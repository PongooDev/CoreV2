#include "pch.h"
#include "Player/FortPlayerController.h"
#include "Building/BuildingSMActor.h"
#include "Engine/World.h"
#include "Finder.h"
#include "FortAssetManager.h"
#include "FortGameData.h"
#include "GameplayTagContainer.h"
#include "Hooks.h"
#include "Items/FortInventory.h"
#include "Items/FortItemDefinition.h"
#include "Log.h"
#include "Natives.h"
#include "ObjectLayout.h"
#include "Pawns/FortPawn.h"
#include "Perception/AISense_Hearing.h"
#include "Player/FortPlayerState.h"
#include "ReflectionParams.h"
#include "Templates/Casts.h"
#include "UObject/UObjectIterator.h"
#include "Weapons/FortWeapon.h"

#include <cstring>
#include <tuple>

// 1.8.1 (CL-3729133) strips AFortPlayerController's six building RPC bodies: ServerCreate/Edit/BeginEditing/EndEditing/
// Repair/UpgradeBuildingActor_Implementation (slots 0x1EF-0x1FA) all hold the shared empty function, so the client's
// pieces, edits and repairs never reached the world (#123). Everything those bodies call is still there. These follow
// 1.8's bodies (1.8 0x845D70, 0x846A20, 0x8456A0, 0x846C90, 0x847840, 0x849280) and call the same helpers through the
// slots Finder::ResolveBuildingRestorationLive finds. They leave out 1.8's analytics event ("Building.BuildingAction"),
// its messages and feedback to the client when the server refuses (the client asks the same questions before it sends
// the RPC and shows its own), and two cosmetic montages after a piece is placed (the build tool's and the pawn's).

namespace
{
    // 1.8 reports the builder's noise to the AI's hearing at the piece: loudness 1, range 1000 (0x30B8B0).
    constexpr float BuildingNoiseLoudness = 1.0f;
    constexpr float BuildingNoiseRange = 1000.0f;

    UObject* AuthGameModeOf(const AActor* Actor)
    {
        UWorld* World = Actor ? Actor->GetWorld() : nullptr;
        return World ? reinterpret_cast<UObject*>(World->GetAuthGameMode()) : nullptr;
    }

    // Where 1.8 takes a piece's location: its root component's, or the zero vector.
    FVector PieceLocation(const AActor* Actor)
    {
        return Actor && Actor->GetRootComponent() ? Actor->GetTransform().GetLocation() : FVector();
    }

    // A gameplay tag is its name; Die takes it by value in a register.
    uint64_t TagBits(const wchar_t* TagName)
    {
        const FGameplayTag Tag{ FName(TagName) };
        uint64_t Bits = 0;
        static_assert(sizeof(Tag) == sizeof(Bits));
        std::memcpy(&Bits, &Tag, sizeof(Bits));
        return Bits;
    }

    UFortWorldItem* FindItemForDefinition(AFortInventory* Inventory, const UFortItemDefinition* ItemDefinition)
    {
        if (!Inventory || !ItemDefinition)
        {
            return nullptr;
        }
        TArray<UFortWorldItem*>& Instances = Inventory->Inventory.ItemInstances;
        for (int32_t Index = 0; Index < Instances.Num(); ++Index)
        {
            UFortWorldItem* Item = Instances[Index];
            if (Item && static_cast<UFortItemDefinition*>(Item->ItemEntry.ItemDefinition) == ItemDefinition)
            {
                return Item;
            }
        }
        return nullptr;
    }

    APlayerState* PlayerStateOf(const AFortPlayerController* Controller)
    {
        return static_cast<APlayerState*>(Controller->PlayerState);
    }

    AFortPlayerStateZone* EditingPlayerOf(ABuildingSMActor* Building)
    {
        return CallVirtual<AFortPlayerStateZone*>(Building, Natives::ABuildingSMActor_GetEditingPlayerIndex);
    }

    void SetEditingPlayer(ABuildingSMActor* Building, AFortPlayerStateZone* EditingPlayer)
    {
        CallVirtual<void>(Building, Natives::ABuildingSMActor_SetEditingPlayerIndex, EditingPlayer);
    }
}

void AFortPlayerController::HookBuildingVTables()
{
    UObject* PlayerControllerDefault = StaticClass()->GetDefaultObject(false);
    void** PlayerControllerVTable = PlayerControllerDefault ? *reinterpret_cast<void***>(PlayerControllerDefault) : nullptr;
    const std::tuple<UFunction*, void*, const wchar_t*> Restorations[] = {
        { StaticFunction_ServerCreateBuildingActor(), reinterpret_cast<void*>(&ServerCreateBuildingActor_ImplementationHK), L"AFortPlayerController::ServerCreateBuildingActor_Implementation" },
        { StaticFunction_ServerEditBuildingActor(), reinterpret_cast<void*>(&ServerEditBuildingActor_ImplementationHK), L"AFortPlayerController::ServerEditBuildingActor_Implementation" },
        { StaticFunction_ServerBeginEditingBuildingActor(), reinterpret_cast<void*>(&ServerBeginEditingBuildingActor_ImplementationHK), L"AFortPlayerController::ServerBeginEditingBuildingActor_Implementation" },
        { StaticFunction_ServerEndEditingBuildingActor(), reinterpret_cast<void*>(&ServerEndEditingBuildingActor_ImplementationHK), L"AFortPlayerController::ServerEndEditingBuildingActor_Implementation" },
        { StaticFunction_ServerRepairBuildingActor(), reinterpret_cast<void*>(&ServerRepairBuildingActor_ImplementationHK), L"AFortPlayerController::ServerRepairBuildingActor_Implementation" },
        { StaticFunction_ServerUpgradeBuildingActor(), reinterpret_cast<void*>(&ServerUpgradeBuildingActor_ImplementationHK), L"AFortPlayerController::ServerUpgradeBuildingActor_Implementation" },
    };

    // Only where the build's own body is the empty stub: 1.1 to 1.8 keep theirs.
    bool bStripped = false;
    for (const auto& [Function, Implementation, Name] : Restorations)
    {
        const int32_t Index = Finder::ImplementationIndex(Function);
        bStripped = bStripped || (PlayerControllerVTable && Index >= 0 && PlayerControllerVTable[Index] == Natives::SharedEmptyFunction);
    }
    if (!bStripped)
    {
        return;
    }
    if (!Finder::ResolveBuildingRestorationLive(ABuildingSMActor::EditingPlayerOffset()))
    {
        Log::Error(L"Building: this build strips AFortPlayerController's building RPCs and their helpers were not found, so nobody can build");
        return;
    }

    for (const auto& [Function, Implementation, Name] : Restorations)
    {
        const int32_t Index = Finder::ImplementationIndex(Function);
        if (Index < 0)
        {
            Log::Error(L"Finder: {} slot not found", Name);
            continue;
        }
        for (TObjectIterator<UClass> It; It; ++It)
        {
            UClass* Class = *It;
            UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
            void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
            if (VTable && VTable[Index] == Natives::SharedEmptyFunction)
            {
                Hooks::SwapVTable(Name, VTable, Index, Implementation);
            }
        }
    }
}

bool AFortPlayerController::ValidateBuildingClassData(FBuildingClassData& BuildingClassData)
{
    // 1.8's check (0x826370, gone from 1.8.1): a building actor class whose default is at level 0. 1.8 swaps an upgraded
    // class for its level 0 one from the game state's building class lists; clients place level 0 classes, so one that
    // isn't is refused.
    UClass* BuildingClass = BuildingClassData.BuildingClass;
    ABuildingActor* Default = BuildingClass && BuildingClass->IsChildOf(ABuildingActor::StaticClass()) ? Cast<ABuildingActor>(BuildingClass->GetDefaultObject()) : nullptr;
    if (!Default)
    {
        return false;
    }
    if (Default->CurrentBuildingLevel > 0)
    {
        Log::Warning(L"Building: {} asked to build {}, which is at level {}", GetName().ToStdString(), BuildingClass->GetName().ToStdString(), static_cast<int32_t>(Default->CurrentBuildingLevel));
        return false;
    }
    return true;
}

bool AFortPlayerController::HasRequiredBuildingLevel(const FBuildingClassData& BuildingClassData)
{
    // 1.8's check (0x82FD70, gone from 1.8.1): HasRequiredBuildingLevel with the class's resource type.
    UClass* BuildingClass = BuildingClassData.BuildingClass;
    ABuildingSMActor* Default = BuildingClass && BuildingClass->IsChildOf(ABuildingSMActor::StaticClass()) ? Cast<ABuildingSMActor>(BuildingClass->GetDefaultObject()) : nullptr;
    const uint8_t ResourceType = Default ? static_cast<uint8_t>(Default->ResourceType) : 0;
    return Default && CallVirtual<bool>(this, Natives::AFortPlayerController_HasRequiredBuildingLevelIndex, BuildingClassData.UpgradeLevel, ResourceType);
}

void AFortPlayerController::ServerCreateBuildingActor_Implementation(const FBuildingClassData& InBuildingClassData, const FVector& InBuildLoc, const FRotator& InBuildRot, bool bMirrored)
{
#if WITH_SERVER_CODE
    FBuildingClassData BuildingClassData = InBuildingClassData;
    if (!ValidateBuildingClassData(BuildingClassData))
    {
        return;
    }
    UClass* BuildingClass = BuildingClassData.BuildingClass;
    if (!BuildingClass || !BuildingClass->IsChildOf(ABuildingSMActor::StaticClass()))
    {
        return;
    }
    const FGameplayTag InteractBuild{ FName(L"Gameplay.Action.Player.InteractBuild") };
    if (!CallVirtual<bool>(this, Natives::AFortPlayerController_CanPerformActionIndex, &InteractBuild))
    {
        return;
    }

    // The structural grid may snap the location and rotation; the piece goes where it says.
    FVector BuildLoc = InBuildLoc;
    FRotator BuildRot = InBuildRot;
    if (CallVirtual<bool>(this, Natives::AFortPlayerController_IsBuildingRestrictedIndex, &BuildingClassData, &BuildLoc, &BuildRot))
    {
        return;
    }
    FBuildingClassData AffordClassData = BuildingClassData;
    const bool bCanAfford = CallVirtual<bool>(this, Natives::AFortPlayerController_CanAffordToPlaceBuildableClassIndex, &AffordClassData);
    const bool bHasRequiredLevel = HasRequiredBuildingLevel(BuildingClassData);
    TArray<ABuildingActor*> ExistingBuildings;
    const int32_t QueryResult = CallVirtual<int32_t>(this, Natives::AFortPlayerController_CanPlaceBuildableClassInStructuralGridIndex, BuildingClass, &BuildLoc, &BuildRot, bMirrored, &ExistingBuildings);
    UWorld* World = GetWorld();
    if (!bCanAfford || !bHasRequiredLevel || QueryResult != 0 || !World)
    {
        return;
    }

    // Props and trees in the way die as if they lost their support.
    const uint64_t StructuralDeath = TagBits(L"Gameplay.Effect.InstantDeath.StructuralSupport");
    for (int32_t Index = 0; Index < ExistingBuildings.Num(); ++Index)
    {
        ABuildingActor* Existing = ExistingBuildings[Index];
        if (Existing && Existing->bDestroyOnPlayerBuildingPlacement)
        {
            CallVirtual<void>(Existing, Natives::ABuildingActor_DieIndex, StructuralDeath, static_cast<AController*>(nullptr), static_cast<AActor*>(nullptr));
        }
    }

    FActorSpawnParameters SpawnParameters;
    SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    FTransform SpawnTransform;
    SpawnTransform.Rotation = BuildRot.Quaternion();
    SpawnTransform.SetLocation(BuildLoc);
    ABuildingSMActor* Building = Cast<ABuildingSMActor>(World->SpawnActor(BuildingClass, &SpawnTransform, SpawnParameters));
    if (!Building)
    {
        return;
    }

    // SetCurrentBuildingLevel (slot 0xC3): the level, on the server.
    if (Building->Role == ROLE_Authority)
    {
        Building->CurrentBuildingLevel = BuildingClassData.UpgradeLevel;
    }
    CallVirtual<void>(Building, Natives::ABuildingSMActor_SetMirroredIndex, bMirrored);
    AFortPlayerState* FortPlayerState = Cast<AFortPlayerState>(reinterpret_cast<UObject*>(PlayerStateOf(this)));
    const int32_t WorldPlayerId = FortPlayerState ? static_cast<int32_t>(FortPlayerState->WorldPlayerId) : -1;
    CallVirtual<void>(Building, Natives::ABuildingActor_InitializeBuildingActorIndex, true, WorldPlayerId, static_cast<void*>(nullptr), static_cast<void*>(nullptr), false);
    CallVirtual<void>(Building, Natives::ABuildingActor_PostInitializeSpawnedBuildingActorIndex, true);
    FBuildingClassData PayClassData = BuildingClassData;
    CallVirtual<int32_t>(this, Natives::AFortPlayerController_PayBuildableClassPlacementCostIndex, &PayClassData);

    // The game mode scores the piece with its builder; Battle Royale's gives it the builder's team there (#120).
    if (UObject* GameMode = AuthGameModeOf(this))
    {
        CallVirtual<void>(GameMode, Natives::AFortGameMode_ScoreBuildingConstructionIndex, static_cast<AController*>(this), Building);
    }
    if (AFortPawn* Pawn = MyFortPawn)
    {
        UAISense_Hearing::ReportNoiseEvent(Pawn, PieceLocation(Building), BuildingNoiseLoudness, Pawn, BuildingNoiseRange, FName());
    }
#endif
}

void AFortPlayerController::ServerEditBuildingActor_Implementation(ABuildingSMActor* BuildingActorToEdit, UClass* NewBuildingClass, int32_t RotationIterations, bool bMirrored)
{
#if WITH_SERVER_CODE
    ABuildingSMActor* Building = BuildingActorToEdit;
    if (!Building || !Building->GetWorld() || reinterpret_cast<UObject*>(EditingPlayerOf(Building)) != reinterpret_cast<UObject*>(PlayerStateOf(this)) || Building->bDestroyed)
    {
        return;
    }

    SetEditingPlayer(Building, nullptr);
    // Replacement type 1 (edited): the new piece takes the old one's place and team (ReplaceBuildingActor, 1.8.1 0x4A3760).
    const int32_t BuildingLevel = Building->CurrentBuildingLevel;
    CallVirtual<ABuildingSMActor*>(Building, Natives::ABuildingSMActor_ReplaceBuildingActorIndex, static_cast<uint8_t>(1), NewBuildingClass, BuildingLevel, RotationIterations, bMirrored, this);
    if (UObject* GameMode = AuthGameModeOf(this))
    {
        CallVirtual<void>(GameMode, Natives::AFortGameMode_ScoreBuildingEditIndex, static_cast<AController*>(this), Building);
    }
#endif
}

void AFortPlayerController::ExecuteEditTool(ABuildingSMActor* EditActor)
{
    // The edit tool comes out (UFortGameData::EditToolItem's world item) and edits EditActor, or nothing.
    UFortGameData* GameData = UFortAssetManager::Get().GetGameData();
    UFortWorldItemDefinition* EditTool = GameData ? GameData->EditToolItem.LoadSynchronous() : nullptr;
    UFortWorldItem* Item = FindItemForDefinition(WorldInventory, EditTool);
    if (!Item || !EditTool || !EditTool->ServerExecute(Item, this))
    {
        return;
    }
    AFortPawn* Pawn = MyFortPawn;
    if (AFortWeap_EditingTool* Weapon = Pawn ? Cast<AFortWeap_EditingTool>(static_cast<AFortWeapon*>(Pawn->CurrentWeapon)) : nullptr)
    {
        Weapon->SetEditActor(EditActor);
    }
}

void AFortPlayerController::ServerBeginEditingBuildingActor_Implementation(ABuildingSMActor* BuildingActorToEdit)
{
#if WITH_SERVER_CODE
    ABuildingSMActor* Building = BuildingActorToEdit;
    if (!Building || !MyFortPawn || !CallVirtual<bool>(Building, Natives::ABuildingSMActor_CanBePlayerEditedIndex, this))
    {
        return;
    }
    APlayerState* State = PlayerStateOf(this);
    if (!State)
    {
        return;
    }

    SetEditingPlayer(Building, Cast<AFortPlayerStateZone>(reinterpret_cast<UObject*>(State)));
    ExecuteEditTool(Building);
#endif
}

void AFortPlayerController::ServerEndEditingBuildingActor_Implementation(ABuildingSMActor* BuildingActorToStopEditing)
{
#if WITH_SERVER_CODE
    ABuildingSMActor* Building = BuildingActorToStopEditing;
    if (!Building || !MyFortPawn || reinterpret_cast<UObject*>(EditingPlayerOf(Building)) != reinterpret_cast<UObject*>(PlayerStateOf(this)) || Building->bDestroyed)
    {
        return;
    }

    SetEditingPlayer(Building, nullptr);
    ExecuteEditTool(nullptr);
#endif
}

void AFortPlayerController::FortClientPlaySoundAtLocation(UObject* Sound, const FVector& Location, float VolumeMultiplier, float PitchMultiplier)
{
    Reflection::FParams Params(StaticFunction_FortClientPlaySoundAtLocation());
    Params.Set(L"Sound", Sound);
    Params.Set(L"Location", Location);
    Params.Set(L"VolumeMultiplier", VolumeMultiplier);
    Params.Set(L"PitchMultiplier", PitchMultiplier);
    Params.Call(this);
}

void AFortPlayerController::ServerRepairBuildingActor_Implementation(ABuildingSMActor* BuildingActorToRepair)
{
#if WITH_SERVER_CODE
    ABuildingSMActor* Building = BuildingActorToRepair;
    UWorld* World = Building ? Building->GetWorld() : nullptr;
    if (!World)
    {
        return;
    }

    const bool bNeedsRepair = CallVirtual<bool>(Building, Natives::ABuildingSMActor_NeedsRepairIndex);
    FVector Location = PieceLocation(Building);
    FRotator Rotation = Building->GetActorRotation();
    const bool bHasPermission = CallVirtual<bool>(this, Natives::AFortPlayerController_HasPermissionToEditWorldIndex, static_cast<ABuildingActor*>(Building), &Location, &Rotation);
    if ((!bNeedsRepair && !bHasPermission) || (bHasPermission && CallVirtual<bool>(Building, Natives::ABuildingSMActor_IsBeingPlayerEditedIndex)) || !bNeedsRepair)
    {
        return;
    }
    if (!CallVirtual<bool>(this, Natives::AFortPlayerController_CanAffordToRepairIndex, Building))
    {
        return;
    }

    const int32_t ResourcesSpent = CallVirtual<int32_t>(this, Natives::AFortPlayerController_PayBuildingRepairCostIndex, Building);
    CallVirtual<void>(Building, Natives::ABuildingSMActor_RepairBuildingIndex, this, ResourcesSpent);
    if (UObject* GameMode = AuthGameModeOf(this))
    {
        CallVirtual<void>(GameMode, Natives::AFortGameMode_ScoreBuildingRepairIndex, static_cast<AController*>(this), static_cast<ABuildingActor*>(Building));
    }
    if (AFortPawn* Pawn = MyFortPawn)
    {
        UAISense_Hearing::ReportNoiseEvent(Pawn, Location, BuildingNoiseLoudness, Pawn, BuildingNoiseRange, FName());
    }

    // Every player in the world hears the repair start there (0x5FE470).
    const TAssetPtr<UObject>& RepairSound = StartRepairSound;
    if (UObject* Sound = RepairSound.LoadSynchronous())
    {
        for (TObjectIterator<AFortPlayerController> It; It; ++It)
        {
            if (It->GetWorld() == World && !It->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
            {
                It->FortClientPlaySoundAtLocation(Sound, Location, 1.0f, 1.0f);
            }
        }
    }
#endif
}

void AFortPlayerController::ServerUpgradeBuildingActor_Implementation(ABuildingActor* BuildingActorToUpgrade, int32_t NewUpgradeLevel)
{
#if WITH_SERVER_CODE
    if (BuildingActorToUpgrade && BuildingActorToUpgrade->GetWorld())
    {
        CallVirtual<void>(BuildingActorToUpgrade, Natives::ABuildingSMActor_OnServerAttemptBuildingUpgradeIndex, this, NewUpgradeLevel);
    }
#endif
}

void AFortPlayerController::ServerCreateBuildingActor_ImplementationHK(AFortPlayerController* This, const FBuildingClassData& BuildingClassData, const FVector& BuildLoc, const FRotator& BuildRot, bool bMirrored)
{
    This->ServerCreateBuildingActor_Implementation(BuildingClassData, BuildLoc, BuildRot, bMirrored);
}

void AFortPlayerController::ServerEditBuildingActor_ImplementationHK(AFortPlayerController* This, ABuildingSMActor* BuildingActorToEdit, UClass* NewBuildingClass, int32_t RotationIterations, bool bMirrored)
{
    This->ServerEditBuildingActor_Implementation(BuildingActorToEdit, NewBuildingClass, RotationIterations, bMirrored);
}

void AFortPlayerController::ServerBeginEditingBuildingActor_ImplementationHK(AFortPlayerController* This, ABuildingSMActor* BuildingActorToEdit)
{
    This->ServerBeginEditingBuildingActor_Implementation(BuildingActorToEdit);
}

void AFortPlayerController::ServerEndEditingBuildingActor_ImplementationHK(AFortPlayerController* This, ABuildingSMActor* BuildingActorToStopEditing)
{
    This->ServerEndEditingBuildingActor_Implementation(BuildingActorToStopEditing);
}

void AFortPlayerController::ServerRepairBuildingActor_ImplementationHK(AFortPlayerController* This, ABuildingSMActor* BuildingActorToRepair)
{
    This->ServerRepairBuildingActor_Implementation(BuildingActorToRepair);
}

void AFortPlayerController::ServerUpgradeBuildingActor_ImplementationHK(AFortPlayerController* This, ABuildingActor* BuildingActorToUpgrade, int32_t NewUpgradeLevel)
{
    This->ServerUpgradeBuildingActor_Implementation(BuildingActorToUpgrade, NewUpgradeLevel);
}
