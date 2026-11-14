#include "pch.h"
#include "Player/FortPlayerController.h"
#include "Abilities/FortPlayerAttrSet.h"
#include "Abilities/GameplayAbility.h"
#include "AbilitySystemComponent.h"
#include "Athena/AI/AthenaNavSystem.h"
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
#include "Logging/LogMacros.h"
#include "Natives.h"
#include "ObjectLayout.h"
#include "Pawns/FortPawn.h"
#include "Perception/AISense_Hearing.h"
#include "Player/FortCheatManager.h"
#include "Player/FortPlayerState.h"
#include "ReflectionParams.h"
#include "Templates/Casts.h"
#include "UObject/UObjectIterator.h"
#include "Weapons/FortWeapon.h"

#include <algorithm>
#include <cstring>
#include <tuple>

// 1.8.1 (CL-3729133) strips AFortPlayerController's six building RPC bodies: ServerCreate/Edit/BeginEditing/EndEditing/
// Repair/UpgradeBuildingActor_Implementation (slots 0x1EF-0x1FA) all hold the shared empty function, so the client's
// pieces, edits and repairs never reached the world (#123). Everything those bodies call is still there. These follow
// 1.8's bodies (1.8 0x845D70, 0x846A20, 0x8456A0, 0x846C90, 0x847840, 0x849280) and call the same helpers through the
// slots Finder::ResolveBuildingRestorationLive finds. They leave out 1.8's analytics event ("Building.BuildingAction"),
// its messages and feedback to the client when the server refuses (the client asks the same questions before it sends
// the RPC and shows its own), and two cosmetic montages after a piece is placed (the build tool's and the pawn's).

DEFINE_LOG_CATEGORY_STATIC(LogFortBuilding, Log, All);

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

    // A player's piece is player built (IsPlayerBuilt: an OwnerPersistentID or bPlayerPlaced). InitializeBuildingActor
    // (6.21 0xD29550) sets bPlayerPlaced itself for a placer with a WorldPlayerId, which Save the World's players have and
    // Battle Royale's lack where the Athena game mode's Login doesn't reach AFortGameModeZone::Login (-1, see
    // AFortGameModeAthena::ScoreBuildingConstructionHK; 6.20+ reach it, CoreV2 #443), so such a piece never was. 6.20 and 6.21 keep a setter of the bit alone with no caller left (6.21 0x144F090, 6.20 0x143B650: `or byte ptr
    // [rcx+514h], 40h`), the stripped placement body's. What reads it: AFortPlayspace::ClearPlayspace (6.21 slot 0x136,
    // 0x12F4EC0) kills the player-built pieces in a Playground playspace when its minigame is reset or restarted, and
    // passed over every Battle Royale piece, so a building challenge's pieces stayed (CoreV2 #435); the zone's structure
    // count (InitializeBuildingActor counts a player-built piece, OnDeathServer takes it off), so it is set before
    // InitializeBuildingActor runs; Core's bots, which leave player pieces unharvested.
    void MarkPlayerPlaced(ABuildingSMActor* Building)
    {
        if (Building && ABuildingActor::HasPlayerPlaced())
        {
            Building->bPlayerPlaced = true;
        }
    }

    // Diagnostic for #300 (a player's piece starting at full health): what decides whether a newly placed piece builds up.
    // InitializeBuildingActor (4.1 0xAFE680) sends Gameplay.Effect.Building.Create to the placer's ability system, whose
    // GA_DefaultPlayer_BuildingCreated applies GE_DefaultPlayer_BuildTime to the piece (BuildTime divided by the placer's
    // UFortConstructionSet BuildSpeed). The piece builds up only when its BuildTime is then above 0: it starts at
    // MaxHealth * the initial health percent, with bIsInitiallyBuilding set, and Tick heals it to full.
    void LogPlacedPieceBuildUp(ABuildingSMActor* Building, AFortPlayerState* Placer)
    {
        float BaseBuildTime = -1.0f;
        float BuildTime = -1.0f;
        if (ABuildingActor::HasBuildingAttributeSet() && UFortBuildingActorSet::HasBuildTime())
        {
            if (UFortBuildingActorSet* Set = Building->BuildingAttributeSet)
            {
                const FFortGameplayAttributeData& Attribute = Set->BuildTime;
                BaseBuildTime = Attribute.GetBaseValue();
                BuildTime = Attribute.GetCurrentValue();
            }
        }

        UAbilitySystemComponent* AbilitySystem = Placer ? static_cast<UAbilitySystemComponent*>(Placer->AbilitySystemComponent) : nullptr;
        float BuildSpeed = -1.0f;
        int32_t BuildingCreatedSpecs = 0;
        if (AbilitySystem)
        {
            if (UFortConstructionSet::HasBuildSpeed())
            {
                if (const UFortConstructionSet* ConstructionSet = AbilitySystem->GetSet<UFortConstructionSet>())
                {
                    BuildSpeed = static_cast<const FFortGameplayAttributeData&>(ConstructionSet->BuildSpeed).GetCurrentValue();
                }
            }
            TStructArray<FGameplayAbilitySpec>& Specs = AbilitySystem->ActivatableAbilities.Items;
            for (int32_t Index = 0; Index < Specs.Num(); ++Index)
            {
                UGameplayAbility* Ability = Specs[Index].Ability;
                if (Ability && Ability->GetClass()->GetName().ToStdString() == L"GA_DefaultPlayer_BuildingCreated_C")
                {
                    ++BuildingCreatedSpecs;
                }
            }
        }

        const wchar_t* InitiallyBuilding = ABuildingSMActor::HasIsInitiallyBuilding() ? (Building->bIsInitiallyBuilding ? L"yes" : L"no") : L"unknown";
        UE_LOG(LogFortBuilding, Log, TEXT("%s placed %s: BuildTime %.2f (base %.2f), health %.0f/%.0f, bIsInitiallyBuilding %s; placer BuildSpeed %.3f, %d GA_DefaultPlayer_BuildingCreated"),
            Placer ? *Placer->GetName() : TEXT("nobody"), *Building->GetName(), BuildTime, BaseBuildTime, Building->GetHealth(), Building->GetMaxHealth(), InitiallyBuilding,
            BuildSpeed, BuildingCreatedSpecs);
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
    // 1.8's check (0x826370, gone from 1.8.1): a building actor class. A client places the class of its CurrentResourceLevel
    // (1.9 0x848740), which BuildWith sets ("buildwith metal 3", #155): Save the World has a class per level (PBWA_M1 to
    // M3...), whose default is at that level, and the client sends UpgradeLevel 0 with it (1.9 0x847480). 1.8 swaps such a
    // class for its level 0 one from the game state's lists; Core places the class the client previewed, at its level, so
    // the piece is the one its build preview showed, and HasRequiredBuildingLevel checks that level.
    UClass* BuildingClass = BuildingClassData.BuildingClass;
    ABuildingActor* Default = BuildingClass && BuildingClass->IsChildOf(ABuildingActor::StaticClass()) ? Cast<ABuildingActor>(BuildingClass->GetDefaultObject()) : nullptr;
    if (!Default)
    {
        return false;
    }
    BuildingClassData.UpgradeLevel = std::max<int32_t>(BuildingClassData.UpgradeLevel, Default->CurrentBuildingLevel);
    return true;
}

bool AFortPlayerController::HasRequiredBuildingLevel(const FBuildingClassData& BuildingClassData)
{
    // 1.8's check (0x82FD70, gone from 1.8.1): HasRequiredBuildingLevel with the class's resource type, true below the
    // player's MaxBuildLevel for it (1.9 0x843E00), which Save the World's research raises. A level the player's
    // "cheat buildwith" chose is theirs to build too (#155).
    UClass* BuildingClass = BuildingClassData.BuildingClass;
    ABuildingSMActor* Default = BuildingClass && BuildingClass->IsChildOf(ABuildingSMActor::StaticClass()) ? Cast<ABuildingSMActor>(BuildingClass->GetDefaultObject()) : nullptr;
    const uint8_t ResourceType = Default ? static_cast<uint8_t>(Default->ResourceType) : 0;
    if (!Default)
    {
        return false;
    }
    if (CallVirtual<bool>(this, Natives::AFortPlayerController_HasRequiredBuildingLevelIndex, BuildingClassData.UpgradeLevel, ResourceType))
    {
        return true;
    }
    return BuildingClassData.UpgradeLevel > 0 && BuildingClassData.UpgradeLevel <= UFortCheatManager::BuildWithLevel(this);
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
    MarkPlayerPlaced(Building);
    CallVirtual<void>(Building, Natives::ABuildingActor_InitializeBuildingActorIndex, EFortBuildingInitializationReason::Spawned, WorldPlayerId, static_cast<void*>(nullptr), static_cast<void*>(nullptr), false);
    CallVirtual<void>(Building, Natives::ABuildingActor_PostInitializeSpawnedBuildingActorIndex, EFortBuildingInitializationReason::Spawned);
    LogPlacedPieceBuildUp(Building, FortPlayerState);
    UAthenaNavSystem::AddDirtyArea(Building);
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
    const bool bWasPlayerPlaced = ABuildingActor::HasPlayerPlaced() && Building->bPlayerPlaced;
    UAthenaNavSystem::AddDirtyArea(Building);
    ABuildingSMActor* NewBuilding = CallVirtual<ABuildingSMActor*>(Building, Natives::ABuildingSMActor_ReplaceBuildingActorIndex, static_cast<uint8_t>(1), NewBuildingClass, BuildingLevel, RotationIterations, bMirrored, this);
    // The edited piece is still the player's (ReplaceBuildingActor initializes it with the old one's OwnerPersistentID,
    // -1 in Battle Royale, so it doesn't carry the flag over itself).
    if (bWasPlayerPlaced)
    {
        MarkPlayerPlaced(NewBuilding);
    }
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

    // Every player in the world hears the repair start there (0x5FE470). 5.21 (CL-4288479) moves StartRepairSound to
    // ABuildingSMActor, which plays it itself at its location when the repair starts (0xCA2AE0).
    if (!Member_StartRepairSound.IsAvailable())
    {
        return;
    }
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
