#include "pch.h"
#include "Creative/FortAthenaCreativePortal.h"
#include "Athena/FortGameModeAthena.h"
#include "Athena/FortPlayerControllerAthena.h"
#include "Creative/FortCreativePortalManager.h"
#include "Creative/FortLevelSaveComponent.h"
#include "Creative/FortVolume.h"
#include "Finder.h"
#include "GameFramework/Controller.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerState.h"
#include "GameplayTagContainer.h"
#include "Hooks.h"
#include "Items/FortItemDefinition.h"
#include "Kismet/GameplayStatics.h"
#include "Log.h"
#include "Natives.h"
#include "Pawns/FortPlayerPawn.h"
#include "PlaysetLevelStreamComponent.h"
#include "Player/FortPlayerState.h"
#include "ReflectionParams.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"

#include <algorithm>
#include <format>

namespace
{
    // The island's spawn the rift sends a pawn to: from 7.10 a Player Spawner set as an island start (#485: the start
    // the game's own respawn place gives a player on the island outside a game, AFortVolume::FindIslandStart), else
    // (bUseSpawnTags) its Creative.Island.Spawn start, else any start of the island, else over the middle of the volume.
    bool LinkedVolumeSpawn(AFortVolume* Volume, bool bUseSpawnTags, const FVector& From, FVector& OutLocation, FRotator& OutRotation, const wchar_t*& OutWhat, bool& bOutInAir)
    {
        bOutInAir = false;
        AFortPlayerStartCreative* Start = Volume->FindIslandStart();
        OutWhat = Start ? L"its island start" : L"its spawn";
        if (!Start && bUseSpawnTags)
        {
            Start = Volume->FindPlayerStart(L"Creative.Island.Spawn", From);
        }
        if (!Start)
        {
            Start = Volume->FindPlayerStart(nullptr, From);
        }
        if (Start)
        {
            const FTransform Transform = Start->GetTransform();
            OutLocation = Transform.GetLocation();
            OutRotation = FRotator(0.0f, Transform.Rotation.Rotator().Yaw, 0.0f);
            return true;
        }
        FVector Origin;
        FVector Extent;
        Volume->GetActorBounds(false, Origin, Extent);
        if (Extent.X <= 0.0f || Extent.Y <= 0.0f)
        {
            return false;
        }
        OutLocation = FVector(Origin.X, Origin.Y, Origin.Z + Extent.Z);
        OutRotation = FRotator(0.0f, 0.0f, 0.0f);
        OutWhat = L"the air over it";
        bOutInAir = true;
        return true;
    }

    // The rift's skydive (#497): Abilities.Generic.Rift.Trigger, the event GA_Rift_Athena_Skydive triggers on (granted
    // with GAS_DefaultPlayer), as BGA_IslandPortal sends it to a pawn it puts in the air (its NotifyTeleportedPlayerPawn
    // with StartSkydiving, TeleportToLobbyIsland, a vehicle's passengers).
    void TriggerRiftSkydive(AFortPlayerPawn* PlayerPawn)
    {
        static UFunction* Send = FindObject<UFunction>(nullptr, L"/Script/GameplayAbilities.AbilitySystemBlueprintLibrary:SendGameplayEventToActor");
        static UClass* LibraryClass = FindObject<UClass>(nullptr, L"/Script/GameplayAbilities.AbilitySystemBlueprintLibrary");
        UObject* Library = LibraryClass ? LibraryClass->GetDefaultObject() : nullptr;
        const FName TagName(L"Abilities.Generic.Rift.Trigger", FNAME_Find);
        Reflection::FParams Event(Send, true);
        if (!Library || TagName.IsNone() || !Event.Set(L"Actor", reinterpret_cast<AActor*>(PlayerPawn)) || !Event.Set(L"EventTag", FGameplayTag{ TagName }) || !Event.Call(Library))
        {
            Log::Warning(L"Creative: the rift's skydive wasn't triggered ({})", TagName.IsNone() ? L"no Abilities.Generic.Rift.Trigger tag" : L"no SendGameplayEventToActor");
        }
    }
}

UFortLevelSaveComponent* AFortAthenaCreativePortal::GetLevelSaveComponent() const
{
    AFortVolume* Volume = LinkedVolume;
    return UFortLevelSaveComponent::GetForVolume(reinterpret_cast<AActor*>(Volume));
}

void AFortAthenaCreativePortal::RefreshPortal()
{
    if (Natives::AFortAthenaCreativePortal_RefreshPortal)
    {
        Natives::AFortAthenaCreativePortal_RefreshPortal(this);
    }
}

void AFortAthenaCreativePortal::SetOwningPlayerState(AFortPlayerState* NewOwner)
{
    // A replicated, RepNotify member (OnRep_OwningPlayerState: the rift's name plate and who may use it). The server
    // calls its own OnRep as Epic's server code does after setting one.
    if (Member_OwningPlayerState.IsAvailable())
    {
        OwningPlayerState = NewOwner;
        if (UFunction* OnRep = FindStaticFunction_OnRep_OwningPlayerState())
        {
            Reflection::FParams Params(OnRep, true);
            Params.Call(this);
        }
    }
    // 7.10: the owner's id (OnRep_OwningPlayer), copied through the build's own struct. No owner leaves it as it is:
    // ReleasePortal clears it.
    else if (Member_OwningPlayer.IsAvailable() && NewOwner)
    {
        UScriptStruct* NetIdStruct = FUniqueNetIdRepl::StaticStruct();
        const FUniqueNetIdRepl& Id = reinterpret_cast<APlayerState*>(NewOwner)->UniqueId;
        if (!NetIdStruct || !Id.IsValid())
        {
            Log::Warning(L"Creative: {} gets no owner: {}", GetName().ToStdString(), !NetIdStruct ? L"FUniqueNetIdRepl wasn't found" : L"the player has no id");
            return;
        }
        NetIdStruct->CopyScriptStruct(&static_cast<FUniqueNetIdRepl&>(OwningPlayer), &Id);
        if (UFunction* OnRep = FindStaticFunction_OnRep_OwningPlayer())
        {
            Reflection::FParams Params(OnRep, true);
            Params.Call(this);
        }
    }
    ForceNetUpdate();
}

bool AFortAthenaCreativePortal::HasOwner() const
{
    if (Member_OwningPlayerState.IsAvailable())
    {
        return static_cast<AFortPlayerState*>(OwningPlayerState) != nullptr;
    }
    return Member_OwningPlayer.IsAvailable() && static_cast<const FUniqueNetIdRepl&>(OwningPlayer).IsValid();
}

bool AFortAthenaCreativePortal::IsOwnedBy(const AFortPlayerState* PlayerState) const
{
    if (!PlayerState)
    {
        return false;
    }
    if (Member_OwningPlayerState.IsAvailable())
    {
        return static_cast<AFortPlayerState*>(OwningPlayerState) == PlayerState;
    }
    if (!Member_OwningPlayer.IsAvailable())
    {
        return false;
    }
    const FUniqueNetIdRepl& Owner = OwningPlayer;
    const FUniqueNetIdRepl& Id = reinterpret_cast<const APlayerState*>(PlayerState)->UniqueId;
    return Owner.IsValid() && Id.IsValid() && Owner.ToString().ToStdString() == Id.ToString().ToStdString();
}

void AFortAthenaCreativePortal::NotifyTeleportedPlayerPawn(AFortPlayerPawn* PlayerPawn)
{
    // The rift's Blueprint event: its teleport effects and the quest update.
    Reflection::FParams Params(FindStaticFunction_NotifyTeleportedPlayerPawn(), true);
    if (Params.Set(L"PlayerPawn", PlayerPawn))
    {
        Params.Call(this);
    }
}

void AFortAthenaCreativePortal::TeleportPlayer(AFortPlayerPawn* PlayerPawn, const FRotator& TeleportRotation)
{
#if WITH_SERVER_CODE
    // To the rift's destination: its DestinationActor, else its TeleportLocation.
    if (!PlayerPawn || Role != ROLE_Authority)
    {
        return;
    }
    AActor* Destination = DestinationActor;
    const FVector Location = Destination ? Destination->GetTransform().GetLocation() : static_cast<FVector>(TeleportLocation);
    if (!Destination && Location.X == 0.0f && Location.Y == 0.0f && Location.Z == 0.0f)
    {
        Log::Warning(L"Creative: {} has no destination actor or linked volume", GetName().ToStdString());
        return;
    }
    AActor* PawnActor = reinterpret_cast<AActor*>(PlayerPawn);
    if (PawnActor->TeleportTo(Location, TeleportRotation))
    {
        NotifyTeleportedPlayerPawn(PlayerPawn);
    }
    Log::Info(L"Creative: {} through {} to ({:.0f}, {:.0f}, {:.0f})", PawnActor->GetName().ToStdString(), GetName().ToStdString(), Location.X, Location.Y, Location.Z);
#endif
}

void AFortAthenaCreativePortal::TeleportPlayerToLinkedVolume(AFortPlayerPawn* PlayerPawn, bool bUseSpawnTags)
{
#if WITH_SERVER_CODE
    AFortVolume* Volume = LinkedVolume;
    if (!PlayerPawn || !Volume || Role != ROLE_Authority)
    {
        return;
    }
    AActor* PawnActor = reinterpret_cast<AActor*>(PlayerPawn);
    FVector Location;
    FRotator Rotation;
    const wchar_t* What = L"";
    bool bInAir = false;
    if (!LinkedVolumeSpawn(Volume, bUseSpawnTags, GetTransform().GetLocation(), Location, Rotation, What, bInAir))
    {
        Log::Warning(L"Creative: {} has no spawn on its island {}", GetName().ToStdString(), Volume->GetName().ToStdString());
        return;
    }
    // A far island goes through the travel screen (#497), as TeleportVehicle sends a vehicle's players (7.20 0xCB26E0):
    // further than MaxInstantTeleportDistance (in 2D), the destination is stored, the client hears
    // ClientStartRespawnPreparation (CreativePortal, ClientIslandTravelText) and the pawn moves once it is ready.
    AController* Controller = reinterpret_cast<APawn*>(PlayerPawn)->Controller;
    AFortPlayerControllerAthena* PlayerController = Cast<AFortPlayerControllerAthena>(reinterpret_cast<UObject*>(Controller));
    const FVector From = PawnActor->GetTransform().GetLocation();
    const float MaxInstant = Member_MaxInstantTeleportDistance.IsAvailable() ? static_cast<float>(MaxInstantTeleportDistance) : 0.0f;
    const float DistanceSquared2D = (Location.X - From.X) * (Location.X - From.X) + (Location.Y - From.Y) * (Location.Y - From.Y);
    if (PlayerController && DistanceSquared2D > MaxInstant * MaxInstant)
    {
        // Already on the way: the Blueprint holds the player off its rifts meanwhile (bDelayedTeleporting).
        if (PlayerController->IsDelayedTeleporting())
        {
            return;
        }
        if (PlayerController->StartDelayedTeleport(Location, Rotation, L"CreativePortal", PlayerController->GetClientIslandTravelText()))
        {
            const TWeakObjectPtr<AFortAthenaCreativePortal> WeakThis(this);
            const TWeakObjectPtr<AFortPlayerPawn> WeakPawn(PlayerPawn);
            const TWeakObjectPtr<AFortVolume> WeakVolume(Volume);
            PlayerController->FinishDelayedTeleportWhenReady(
                [WeakThis, WeakPawn, WeakVolume, bInAir, What](AFortPlayerControllerAthena* Ready, const FVector& ReadyLocation, const FRotator& ReadyRotation)
                {
                    AFortAthenaCreativePortal* Portal = WeakThis.Get();
                    AFortPlayerPawn* Pawn = WeakPawn.Get();
                    AFortVolume* Island = WeakVolume.Get();
                    if (Portal && Pawn && Island && reinterpret_cast<APawn*>(Pawn)->Controller == reinterpret_cast<AController*>(Ready))
                    {
                        Portal->FinishTeleportToLinkedVolume(Pawn, Island, ReadyLocation, ReadyRotation, bInAir, What);
                    }
                });
            return;
        }
    }
    FinishTeleportToLinkedVolume(PlayerPawn, Volume, Location, Rotation, bInAir, What);
#endif
}

void AFortAthenaCreativePortal::FinishTeleportToLinkedVolume(AFortPlayerPawn* PlayerPawn, AFortVolume* Volume, const FVector& Location, const FRotator& Rotation, bool bInAir, const wchar_t* What)
{
#if WITH_SERVER_CODE
    AActor* PawnActor = reinterpret_cast<AActor*>(PlayerPawn);
    if (PawnActor->TeleportTo(Location, Rotation))
    {
        // The player is on this island now (#485): their controller's CreativePlotLinkedVolume, through the game's own
        // setter, whose server calls with an island 7.00 to 7.20 strip. It ties them to the island's state (their
        // OnMinigameVolumeStateChanged) and counts them on its rifts (CurrentPopulation counts the controllers linked
        // to a rift's island); the hub takes the link back (ServerTeleportToPlaygroundLobbyIsland).
        AController* Controller = reinterpret_cast<APawn*>(PlayerPawn)->Controller;
        if (AFortPlayerControllerAthena* PlayerController = Cast<AFortPlayerControllerAthena>(reinterpret_cast<UObject*>(Controller)))
        {
            PlayerController->SetCreativePlotLinkedVolume(Volume);
        }
        // No Player Spawner or start to land on: the pawn is in the air over the island, and falls as from a rift (#497).
        if (bInAir)
        {
            TriggerRiftSkydive(PlayerPawn);
        }
        // 7.20's OnPlayerPawnTeleported (intact, 0xCAA200) takes the pawn off the rift's TeleportingPawns and calls the
        // Blueprint's NotifyTeleportedPlayerPawn (the exit effects and the quest); 7.00 and 7.10 have only the event.
        if (UFunction* OnTeleported = FindStaticFunction_OnPlayerPawnTeleported())
        {
            Reflection::FParams Params(OnTeleported, true);
            if (Params.Set(L"PlayerPawn", PlayerPawn))
            {
                Params.Call(this);
            }
        }
        else
        {
            NotifyTeleportedPlayerPawn(PlayerPawn);
        }
    }
    Log::Info(L"Creative: {} through {} onto the island {} at {} ({:.0f}, {:.0f}, {:.0f})", PawnActor->GetName().ToStdString(), GetName().ToStdString(), Volume->GetName().ToStdString(),
        What, Location.X, Location.Y, Location.Z);
#endif
}

void AFortAthenaCreativePortal::LoadPlotForUser(const FUniqueNetIdRepl& UserId, UFortCreativeRealEstatePlotItem* PlotItem)
{
#if WITH_SERVER_CODE
    // ServerLoadPlotForPortal_Implementation checked the rift is the player's (or no one's), found PlotItem in their
    // creative profile and calls this with their id; after it, it saves the plot as their last used one
    // (SetLastUsedCreativePlot, McpDedicatedServerCommandUrl). The rift is theirs from now, and its island loads.
    UFortLevelSaveComponent* SaveComponent = GetLevelSaveComponent();
    if (!SaveComponent || Role != ROLE_Authority)
    {
        Log::Warning(L"Creative: {} can't load {}'s island: {}", GetName().ToStdString(), UserId.ToString().ToStdString(),
            !static_cast<AFortVolume*>(LinkedVolume) ? L"it has no linked volume" : L"its volume has no UFortLevelSaveComponent");
        return;
    }
    // Shut while the island loads: CloudLoadFinished opens it (the rift's FinishedReadingCloudFiles, 0xBAF020).
    bPortalOpen = false;
    if (UFunction* OnRep = FindStaticFunction_OnRep_PortalOpen())
    {
        Reflection::FParams Params(OnRep, true);
        Params.Call(this);
    }
    SaveComponent->LoadPlotForUser(UserId, PlotItem);
#endif
}

void AFortAthenaCreativePortal::LoadPlotForUserHK(AFortAthenaCreativePortal* This, const FUniqueNetIdRepl& UserId, UFortCreativeRealEstatePlotItem* PlotItem)
{
    if (This)
    {
        This->LoadPlotForUser(UserId, PlotItem);
    }
}

void AFortAthenaCreativePortal::LoadPlotFromMnemonicForUser(const FUniqueNetIdRepl& UserId, const FString& Mnemonic)
{
#if WITH_SERVER_CODE
    // ServerLoadPlotForPortalFromMnemonic_Implementation (7.10 0xC84E10) checked the rift is no other player's and has
    // the player's id: the code's island loads into the rift's island space (PongooDev/CoreV2#488), the way the featured
    // rifts load theirs. The component's own load rejects what it can't take ("Rejecting plot load for mnemonic %s: %s":
    // not_authorized, invalid_mnemonic, queue_busy, cloudstorage_unavailable), and the links service's answer a code
    // that isn't published.
    UFortLevelSaveComponent* SaveComponent = GetLevelSaveComponent();
    if (!SaveComponent || Role != ROLE_Authority)
    {
        Log::Warning(L"Creative: {} can't load the island {}: {}", GetName().ToStdString(), Mnemonic.ToStdString(),
            !static_cast<AFortVolume*>(LinkedVolume) ? L"it has no linked volume" : L"its volume has no UFortLevelSaveComponent");
        return;
    }
    // Shut while the island loads, as for a player's island: the load's end opens it.
    bPortalOpen = false;
    if (UFunction* OnRep = FindStaticFunction_OnRep_PortalOpen())
    {
        Reflection::FParams Params(OnRep, true);
        Params.Call(this);
    }
    Log::Info(L"Creative: {} loads the island {} into {}", UserId.ToString().ToStdString(), Mnemonic.ToStdString(), GetName().ToStdString());
    SaveComponent->LoadPlotFromMnemonic(UserId, Mnemonic);
#endif
}

void AFortAthenaCreativePortal::LoadPlotFromMnemonicForUserHK(AFortAthenaCreativePortal* This, const FUniqueNetIdRepl& UserId, const FString& Mnemonic)
{
    if (This)
    {
        This->LoadPlotFromMnemonicForUser(UserId, Mnemonic);
    }
}

DEFINE_FUNCTION(AFortAthenaCreativePortal::execTeleportPlayer)
{
    // 7.00's exec (0x14D4580): the pawn, then the rotation, as the stripped body takes them.
    P_GET_OBJECT(AFortPlayerPawn, Z_Param_PlayerPawn);
    P_GET_STRUCT(FRotator, Z_Param_TeleportRotation);
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<AFortAthenaCreativePortal*>(Context)->TeleportPlayer(Z_Param_PlayerPawn, Z_Param_TeleportRotation);
    P_NATIVE_END;
}

DEFINE_FUNCTION(AFortAthenaCreativePortal::execTeleportPlayerToLinkedVolume)
{
    P_GET_OBJECT(AFortPlayerPawn, Z_Param_PlayerPawn);
    P_GET_UBOOL(Z_Param_bUseSpawnTags);
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<AFortAthenaCreativePortal*>(Context)->TeleportPlayerToLinkedVolume(Z_Param_PlayerPawn, Z_Param_bUseSpawnTags);
    P_NATIVE_END;
}

void AFortAthenaCreativePortal::HookNatives()
{
#if WITH_SERVER_CODE
    if (!StaticClass())
    {
        return;
    }
    if (!Natives::FFrame_Step && !Finder::ResolveScriptFrame())
    {
        Log::Error(L"Hook AFortAthenaCreativePortal natives: the script frame's Step wasn't found");
        return;
    }
    // The two teleports: their execs call the shared empty function on 7.00 (TeleportPlayer 0x14D4580, and
    // TeleportPlayerToLinkedVolume 0x14D46D0, both to 0x29E850).
    const auto Restore = [](UFunction* Function, FNativeFuncPtr Native, const wchar_t* Name)
    {
        uint8_t* Call = Function ? Finder::ImplementationCall(Function) : nullptr;
        if (!Call)
        {
            Log::Error(L"Finder: AFortAthenaCreativePortal::{}'s native call not found", Name);
            return;
        }
        if (!Finder::IsStubFunction(Finder::RelativeTarget(reinterpret_cast<uintptr_t>(Call), 1, 5)))
        {
            Log::Info(L"Finder: AFortAthenaCreativePortal::{} is native in this build", Name);
            return;
        }
        Function->SetNativeFunc(Native);
        Log::Info(L"Hook AFortAthenaCreativePortal::{}: native", Name);
    };
    Restore(FindStaticFunction_TeleportPlayer(), &execTeleportPlayer, L"TeleportPlayer");
    Restore(FindStaticFunction_TeleportPlayerToLinkedVolume(), &execTeleportPlayerToLinkedVolume, L"TeleportPlayerToLinkedVolume");
#endif
}

AFortCreativePortalManager* AFortCreativePortalManager::Get(const UObject* WorldContextObject)
{
    // One per world (AFortGameStateAthena::CreativePortalManager on 7.00; GetCreativePortalManager's native 0xC11A50).
    UClass* Class = StaticClass();
    const TArray<AActor*> Managers = Class && WorldContextObject ? UGameplayStatics::GetAllActorsOfClass(WorldContextObject, Class) : TArray<AActor*>();
    return Managers.Num() > 0 ? static_cast<AFortCreativePortalManager*>(Managers[0]) : nullptr;
}

void AFortCreativePortalManager::LogHub(const UObject* WorldContextObject)
{
#if WITH_SERVER_CODE
    AFortCreativePortalManager* Manager = Get(WorldContextObject);
    if (!Manager)
    {
        Log::Warning(L"Creative: the match has no AFortCreativePortalManager, so no player gets a rift");
        return;
    }
    const TArray<AFortAthenaCreativePortal*>& All = Manager->AllPortals;
    int32_t HubRifts = 0;
    int32_t WithIsland = 0;
    int32_t WithSave = 0;
    int32_t WithPlaysetStream = 0;
    // Hub rifts in their island's LinkedPortals, the list the CreativePlotLinkedVolume setter refreshes (#485).
    int32_t InIslandList = 0;
    for (AFortAthenaCreativePortal* Portal : All)
    {
        if (!Portal || Portal->bReturnToCreativeHub)
        {
            continue;
        }
        ++HubRifts;
        AFortVolume* Volume = Portal->LinkedVolume;
        if (Volume)
        {
            ++WithIsland;
            if (AFortVolume::HasLinkedPortals())
            {
                const TArray<AFortAthenaCreativePortal*>& Listed = Volume->LinkedPortals;
                InIslandList += std::find(Listed.begin(), Listed.end(), Portal) != Listed.end() ? 1 : 0;
            }
        }
        UFortLevelSaveComponent* SaveComponent = Portal->GetLevelSaveComponent();
        if (SaveComponent)
        {
            ++WithSave;
        }
        // The island's own level streams in with its plot (LoadPlotForUser's playset steps).
        if (SaveComponent && SaveComponent->LoadsPlaysetFromPlot() && UPlaysetLevelStreamComponent::GetForActor(reinterpret_cast<AActor*>(Volume)))
        {
            ++WithPlaysetStream;
        }
    }
    const TArray<AFortAthenaCreativePortal*>& Available = Manager->AvailablePortals;
    Log::Info(L"Creative: the hub has {} rifts ({} free), {} linked to an island space ({} in its LinkedPortals), {} with its UFortLevelSaveComponent, {} streaming their plot's playset; {} rifts in all with the islands' own",
        HubRifts, Available.Num(), WithIsland, InIslandList, WithSave, WithPlaysetStream, All.Num());
    // The game mode's StartingItems (#485: what a hub loadout would start from; the Creative phone and the pickaxe aren't
    // among them on 7.00).
    if (AFortGameModeAthena* GameMode = AFortGameModeAthena::GetCurrent(WorldContextObject))
    {
        const TStructArray<FItemAndCount>& StartingItems = GameMode->StartingItems;
        std::wstring Items;
        for (int32_t Index = 0; Index < StartingItems.Num(); ++Index)
        {
            UFortItemDefinition* Item = StartingItems[Index].Item;
            Items += std::format(L"{}{} x{}", Items.empty() ? L"" : L", ", Item ? Item->GetName().ToStdString() : L"None", static_cast<int32_t>(StartingItems[Index].Count));
        }
        Log::Info(L"Creative: the game mode's StartingItems: {}", Items.empty() ? L"none" : Items);
    }
#endif
}

void AFortCreativePortalManager::MarkPortalUsed(AFortAthenaCreativePortal* PortalUsed)
{
    Reflection::FParams Params(FindStaticFunction_MarkPortalUsed(), true);
    if (Params.Set(L"PortalUsed", PortalUsed))
    {
        Params.Call(this);
    }
}

void AFortCreativePortalManager::ReleasePortal(AFortAthenaCreativePortal* PortalToRelease)
{
    Reflection::FParams Params(FindStaticFunction_ReleasePortal(), true);
    if (Params.Set(L"PortalToRelease", PortalToRelease))
    {
        Params.Call(this);
    }
}

AFortAthenaCreativePortal* AFortCreativePortalManager::ClaimPortal(AFortPlayerControllerAthena* PlayerController)
{
#if WITH_SERVER_CODE
    APlayerState* PlayerState = PlayerController ? static_cast<APlayerState*>(reinterpret_cast<AController*>(PlayerController)->PlayerState) : nullptr;
    if (!PlayerState || Role != ROLE_Authority)
    {
        return nullptr;
    }
    // A player keeps their rift (a respawn, a second possession).
    if (AFortAthenaCreativePortal* Owned = PlayerController->GetOwnedPortal())
    {
        return Owned;
    }
    // The first free rift of the hub: AvailablePortals, the hub's own rifts (RegisterPortal puts the island rifts back to
    // the hub, bReturnToCreativeHub, with the used ones), in the order they registered.
    const TArray<AFortAthenaCreativePortal*>& Available = AvailablePortals;
    AFortAthenaCreativePortal* Portal = nullptr;
    for (AFortAthenaCreativePortal* Candidate : Available)
    {
        if (Candidate && !Candidate->bReturnToCreativeHub && static_cast<AFortVolume*>(Candidate->LinkedVolume) && !Candidate->HasOwner())
        {
            Portal = Candidate;
            break;
        }
    }
    if (!Portal)
    {
        Log::Warning(L"Creative: Out of available portals! ({} has no rift: {} of {} in use)", PlayerState->GetPlayerName().ToStdString(),
            static_cast<const TArray<AFortAthenaCreativePortal*>&>(UsedPortals).Num(), static_cast<const TArray<AFortAthenaCreativePortal*>&>(AllPortals).Num());
        return nullptr;
    }
    Portal->SetOwningPlayerState(reinterpret_cast<AFortPlayerState*>(PlayerState));
    MarkPortalUsed(Portal);
    PlayerController->SetOwnedPortal(Portal);
    Log::Info(L"Creative: {} takes the hub's rift {} (index {}), linked to the island {}", PlayerState->GetPlayerName().ToStdString(), Portal->GetName().ToStdString(),
        static_cast<int32_t>(Portal->PortalIndex), static_cast<AFortVolume*>(Portal->LinkedVolume)->GetName().ToStdString());
    return Portal;
#else
    return nullptr;
#endif
}

void AFortCreativePortalManager::ReleasePlayerPortal(AFortPlayerState* PlayerState)
{
#if WITH_SERVER_CODE
    if (!PlayerState)
    {
        return;
    }
    const TArray<AFortAthenaCreativePortal*> Used = UsedPortals;
    for (AFortAthenaCreativePortal* Portal : Used)
    {
        if (!Portal || !Portal->IsOwnedBy(PlayerState))
        {
            continue;
        }
        // The island's other players go back to the hub before it unloads (#664). The server half that did it is
        // compiled out: 8.20 (CL-5625478) has ServerReleasePortal / ServerTeleportToPlaygroundLobbyIsland as nullsubs
        // (slots 0x427 / 0x455), and nothing in the reset (ResetVolumeAndLevelRecord 0x102DB00) or the portal's
        // OnUnloadPlotComplete (0xE1F790) moves a pawn, so they were left standing in the emptied island.
        AFortVolume* Island = Portal->LinkedVolume;
        AFortVolumeManager* VolumeManager = Island ? AFortVolumeManager::Get(Portal) : nullptr;
        const TArray<AController*> Players = VolumeManager ? VolumeManager->GetAllPlayersInVolume(Island) : TArray<AController*>();
        for (AController* Controller : Players)
        {
            AFortPlayerControllerAthena* Player = Controller && !Controller->IsPendingKill() ? Cast<AFortPlayerControllerAthena>(reinterpret_cast<UObject*>(Controller)) : nullptr;
            APlayerState* State = Player ? static_cast<APlayerState*>(Controller->PlayerState) : nullptr;
            if (!State || State == reinterpret_cast<APlayerState*>(PlayerState) || State->IsPendingKill())
            {
                continue;
            }
            Log::Info(L"Creative: {} leaves the island {} for the hub: its owner gave up the rift (left the game or released it)", State->GetPlayerName().ToStdString(), Island->GetName().ToStdString());
            Player->ServerTeleportToPlaygroundLobbyIsland_Implementation();
        }
        if (UFortLevelSaveComponent* SaveComponent = Portal->GetLevelSaveComponent())
        {
            SaveComponent->UnloadPlotForUser();
        }
        // ReleasePortal (0xC1F3B0) clears the rift's owner (0xBBE570) and gives it back to AvailablePortals.
        ReleasePortal(Portal);
        Portal->ForceNetUpdate();
        Log::Info(L"Creative: the rift {} is free again", Portal->GetName().ToStdString());
    }
#endif
}
