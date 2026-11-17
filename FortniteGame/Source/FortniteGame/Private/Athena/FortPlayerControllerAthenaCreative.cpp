#include "pch.h"
#include "Athena/FortGameStateAthena.h"
#include "Athena/FortPlayerControllerAthena.h"
#include "Athena/FortPlaylistAthena.h"
#include "Creative/FortAthenaCreativePortal.h"
#include "Creative/FortCreativePortalManager.h"
#include "Creative/FortLevelSaveComponent.h"
#include "Creative/FortVolume.h"
#include "Engine/World.h"
#include "Finder.h"
#include "FortGameInstance.h"
#include "GameFramework/PlayerState.h"
#include "Hooks.h"
#include "Items/FortInventory.h"
#include "Items/FortItem.h"
#include "Items/FortItemDefinition.h"
#include "Kismet/GameplayStatics.h"
#include "Log.h"
#include "McpProfile.h"
#include "Natives.h"
#include "Player/FortPlayerState.h"
#include "ReflectionParams.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"

#include <algorithm>
#include <format>
#include <limits>
#include <map>
#include <set>
#include <vector>

namespace
{
    std::wstring PlayerNameOf(AFortPlayerControllerAthena* Controller)
    {
        APlayerState* PlayerState = Controller ? static_cast<APlayerState*>(reinterpret_cast<AController*>(Controller)->PlayerState) : nullptr;
        return PlayerState ? PlayerState->GetPlayerName().ToStdString() : std::wstring(L"a player");
    }

    const FUniqueNetIdRepl* UniqueIdOf(AFortPlayerControllerAthena* Controller)
    {
        APlayerState* PlayerState = Controller ? static_cast<APlayerState*>(reinterpret_cast<AController*>(Controller)->PlayerState) : nullptr;
        return PlayerState ? &static_cast<const FUniqueNetIdRepl&>(PlayerState->UniqueId) : nullptr;
    }

    // Swaps the stub at Function's _Implementation slot for Replacement in the controller's class and every class of its
    // with the stub there.
    void RestoreImplementation(UFunction* Function, void* Replacement, const wchar_t* Name)
    {
        UClass* AthenaClass = AFortPlayerControllerAthena::StaticClass();
        const int32_t Index = Function ? Finder::ImplementationIndex(Function) : -1;
        UObject* Default = AthenaClass && Index >= 0 ? AthenaClass->GetDefaultObject(false) : nullptr;
        void** DefaultVTable = Default ? *reinterpret_cast<void***>(Default) : nullptr;
        if (!DefaultVTable)
        {
            Log::Error(L"Finder: AFortPlayerControllerAthena::{}_Implementation slot not found", Name);
            return;
        }
        void* const Stub = DefaultVTable[Index];
        if (!Finder::IsStubFunction(reinterpret_cast<uintptr_t>(Stub)))
        {
            Log::Info(L"Finder: AFortPlayerControllerAthena::{}_Implementation (slot 0x{:X}) is native in this build", Name, Index);
            return;
        }
        const std::wstring HookName = std::format(L"AFortPlayerControllerAthena::{}_Implementation", Name);
        for (TObjectIterator<UClass> It; It; ++It)
        {
            UClass* Class = *It;
            UObject* ClassDefault = Class->IsChildOf(AthenaClass) ? Class->GetDefaultObject(false) : nullptr;
            void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
            if (VTable && VTable[Index] == Stub)
            {
                Hooks::SwapVTable(HookName, VTable, Index, Replacement);
            }
        }
    }

    // The function that contains the code reference to Text (its first), or 0.
    uintptr_t FunctionReferencing(const wchar_t* Text)
    {
        const std::vector<uintptr_t> Refs = Finder::StringRefs(Text);
        return Refs.empty() ? 0 : Finder::FunctionStart(Refs.front());
    }
}

bool AFortPlayerControllerAthena::IsCreativeMatch(const UObject* WorldContextObject)
{
    if (!AFortAthenaCreativePortal::StaticClass() || !UFortPlaylistAthena::HasEnableCreativeMode() || !WorldContextObject)
    {
        return false;
    }
    UWorld* World = WorldContextObject->GetWorld();
    AFortGameStateAthena* GameState = World ? Cast<AFortGameStateAthena>(reinterpret_cast<UObject*>(World->GetGameState())) : nullptr;
    UFortPlaylistAthena* Playlist = GameState && AFortGameStateAthena::HasCurrentPlaylist() ? GameState->GetCurrentPlaylist() : nullptr;
    return Playlist && Playlist->bEnableCreativeMode;
}

void AFortPlayerControllerAthena::SetOwnedPortal(AFortAthenaCreativePortal* Portal, AFortVolume* LinkedVolume)
{
    if (!HasOwnedPortal())
    {
        return;
    }
    // Both replicate to the player: their HUD's island (GetCreativePlotLinkedVolume) and their rift.
    OwnedPortal = Portal;
    CreativePlotLinkedVolume = LinkedVolume;
    ForceNetUpdate();
}

void AFortPlayerControllerAthena::LoadPlotForPortal(AFortAthenaCreativePortal* Portal, const FString& PlotItemId)
{
    if (Portal && Natives::AFortPlayerControllerAthena_ServerLoadPlotForPortal)
    {
        Natives::AFortPlayerControllerAthena_ServerLoadPlotForPortal(this, Portal, PlotItemId);
    }
}

void AFortPlayerControllerAthena::ClaimCreativePortal()
{
#if WITH_SERVER_CODE
    // A player comes into a Creative match: the hub's rift for them, which their last used island loads into (the
    // plot the game keeps as LastUsedCreativeIsland, from the creative profile's last_used_plot; their first island
    // when they have none yet) once LoadCreativePlots has their islands. They can pick another at the rift ("Set Rift
    // Destination", ServerLoadPlotForPortal), from the CreativeIslands it fills.
    if (Role != ROLE_Authority || !IsCreativeMatch(this) || GetOwnedPortal())
    {
        return;
    }
    AFortCreativePortalManager* Manager = AFortCreativePortalManager::Get(this);
    AFortAthenaCreativePortal* Portal = Manager ? Manager->ClaimPortal(this) : nullptr;
    if (!Portal)
    {
        if (!Manager)
        {
            Log::Warning(L"Creative: {} has no rift: the world has no AFortCreativePortalManager", PlayerNameOf(this));
        }
        return;
    }
    UMcpProfile* Profile = GetCreativeProfile();
    if (!Profile)
    {
        UWorld* World = GetWorld();
        UFortGameInstance* GameInstance = World ? Cast<UFortGameInstance>(reinterpret_cast<UObject*>(static_cast<UGameInstance*>(World->OwningGameInstance))) : nullptr;
        Log::Warning(L"Creative: the server has no creative profile for {} (the registration gives one when UFortGameInstance::bCreativeModeProfileEnabled is set: {}), so the "
            L"game can't find their islands' plot items", PlayerNameOf(this),
            GameInstance && UFortGameInstance::HasCreativeModeProfileEnabled() ? (GameInstance->bCreativeModeProfileEnabled ? L"set" : L"not set") : L"unknown");
        return;
    }
    // The profile was queried as the player registered (QueryProfile?profileId=creative); its answer may come before or
    // after this. Its items arriving (and any later change to them) broadcast its ItemsUpdatedDelegate, which loads the
    // islands and the last used one into the rift; an answer that is already in is read now.
    if (UMcpProfile::HasItemsUpdatedDelegate())
    {
        UMcpProfile::FOnItemsUpdated& ItemsUpdated = Profile->ItemsUpdatedDelegate;
        ItemsUpdated.AddUObject(this, &AFortPlayerControllerAthena::OnCreativeProfileItemsUpdated);
    }
    else
    {
        Log::Warning(L"Creative: UMcpProfile's ItemsUpdatedDelegate wasn't found, so {}'s islands are only read now, as they come in", PlayerNameOf(this));
    }
    if (Profile->ProfileRevision < 0)
    {
        Log::Info(L"Creative: {}'s creative profile hasn't come in yet; their islands load into their rift when it does", PlayerNameOf(this));
        return;
    }
    LoadCreativePlots();
    LoadLastUsedPlotIntoPortal();
#endif
}

UMcpProfile* AFortPlayerControllerAthena::GetCreativeProfile() const
{
    UObject* Profile = AFortPlayerController::HasCreativeModeProfile() ? static_cast<UObject*>(CreativeModeProfile) : nullptr;
    return Profile && UMcpProfile::StaticClass() && Profile->IsA(UMcpProfile::StaticClass()) ? static_cast<UMcpProfile*>(Profile) : nullptr;
}

void AFortPlayerControllerAthena::LoadCreativePlots()
{
#if WITH_SERVER_CODE
    // 7.00's client has no code that writes CreativeIslands (only the controller's constructor and destructor touch it)
    // or reads the profile's last_used_plot (its name has no reference): the server's half, which 21.40 still has as
    // AFortPlayerControllerAthena::LoadCreativePlots. The plots are the creative profile's CreativePlot items, which the
    // profile makes with itself as their outer; their order is their number (islandIndex, "Island 1" first).
    UMcpProfile* Profile = GetCreativeProfile();
    if (!Profile || !Member_CreativeIslands.IsAvailable() || !UFortCreativeRealEstatePlotItem::StaticClass())
    {
        return;
    }
    std::vector<UFortCreativeRealEstatePlotItem*> Plots;
    for (TObjectIterator<UFortCreativeRealEstatePlotItem> It; It; ++It)
    {
        if (It->GetOuter() == Profile)
        {
            Plots.push_back(*It);
        }
    }
    std::stable_sort(Plots.begin(), Plots.end(), [](UFortCreativeRealEstatePlotItem* A, UFortCreativeRealEstatePlotItem* B) { return A->IslandIndex < B->IslandIndex; });

    TStructArray<FCreativeIslandData>& Islands = CreativeIslands;
    Islands.Empty();
    std::vector<std::wstring> Ids;
    for (UFortCreativeRealEstatePlotItem* Plot : Plots)
    {
        // An item a later whole-profile answer replaced lingers until the next garbage collection: one island per id.
        const FString Id = Plot->GetItemId();
        if (Id.ToStdString().empty() || std::find(Ids.begin(), Ids.end(), Id.ToStdString()) != Ids.end())
        {
            continue;
        }
        FText Name = Plot->GetIslandDisplayName();
        FCreativeIslandData& Island = Islands.AddDefaulted();
        if (Name.HasTextData())
        {
            Island.IslandName = std::move(Name);
        }
        Island.McpId = Id;
        Ids.push_back(Id.ToStdString());
    }

    // The profile's last_used_plot stat (the server's SetLastUsedCreativePlot sets it as the player picks an island at
    // their rift), when it is one of their islands.
    FString LastUsed;
    const TSharedPtr<FJsonValue, ESPMode::Fast> LastUsedStat = Profile->GetStat(FString(L"last_used_plot"));
    if (LastUsedStat.IsValid())
    {
        LastUsedStat->TryGetString(LastUsed);
    }
    const std::wstring LastUsedId = LastUsed.ToStdString();
    const bool bLastUsedIsIsland = std::find(Ids.begin(), Ids.end(), LastUsedId) != Ids.end();
    if (bLastUsedIsIsland && Member_LastUsedCreativeIsland.IsAvailable())
    {
        LastUsedCreativeIsland = LastUsed;
    }
    ForceNetUpdate();
    Log::Info(L"Creative: {} has {} island(s) in their creative profile (revision {}), last used {}", PlayerNameOf(this), Ids.size(), static_cast<int64_t>(Profile->ProfileRevision),
        bLastUsedIsIsland ? LastUsedId : LastUsedId.empty() ? std::wstring(L"none yet") : std::format(L"{} (not one of them)", LastUsedId));
    if (Ids.empty())
    {
        Log::Warning(L"Creative: {}'s creative profile has no CreativePlot items, so they have no island to load (the backend gives them once the account may play Creative)",
            PlayerNameOf(this));
    }
#endif
}

void AFortPlayerControllerAthena::LoadLastUsedPlotIntoPortal()
{
#if WITH_SERVER_CODE
    AFortAthenaCreativePortal* Portal = GetOwnedPortal();
    AFortVolume* Volume = Portal ? static_cast<AFortVolume*>(Portal->LinkedVolume) : nullptr;
    UFortLevelSaveComponent* SaveComponent = UFortLevelSaveComponent::GetForVolume(reinterpret_cast<AActor*>(Volume));
    if (!Portal || (SaveComponent && static_cast<UFortCreativeRealEstatePlotItem*>(SaveComponent->LoadedPlot)))
    {
        return;
    }
    FString PlotItemId = Member_LastUsedCreativeIsland.IsAvailable() ? static_cast<FString>(LastUsedCreativeIsland) : FString();
    if (PlotItemId.ToStdString().empty() && Member_CreativeIslands.IsAvailable())
    {
        TStructArray<FCreativeIslandData>& Islands = CreativeIslands;
        if (Islands.Num() > 0)
        {
            PlotItemId = Islands[0].McpId;
        }
    }
    if (PlotItemId.ToStdString().empty())
    {
        Log::Info(L"Creative: {}'s rift waits for them to pick an island (they have none)", PlayerNameOf(this));
        return;
    }
    Log::Info(L"Creative: {}'s {} island {} loads into their rift", PlayerNameOf(this),
        Member_LastUsedCreativeIsland.IsAvailable() && !static_cast<FString>(LastUsedCreativeIsland).ToStdString().empty() ? L"last used" : L"first", PlotItemId.ToStdString());
    LoadPlotForPortal(Portal, PlotItemId);
#endif
}

void AFortPlayerControllerAthena::OnCreativeProfileItemsUpdated(int64_t UpdateArgument)
{
#if WITH_SERVER_CODE
    LoadCreativePlots();
    LoadLastUsedPlotIntoPortal();
#endif
}

void AFortPlayerControllerAthena::ReleaseCreativePortal(AFortPlayerState* PlayerState)
{
#if WITH_SERVER_CODE
    AActor* StateActor = reinterpret_cast<AActor*>(PlayerState);
    if (!StateActor || !IsCreativeMatch(StateActor))
    {
        return;
    }
    if (AFortCreativePortalManager* Manager = AFortCreativePortalManager::Get(StateActor))
    {
        Manager->ReleasePlayerPortal(PlayerState);
    }
#endif
}

AFortVolume* AFortPlayerControllerAthena::GetCreativeVolume()
{
    AFortVolume* Own = HasOwnedPortal() ? static_cast<AFortVolume*>(CreativePlotLinkedVolume) : nullptr;
    APawn* Pawn = GetPawn();
    AFortVolumeManager* Manager = Pawn ? AFortVolumeManager::Get(this) : nullptr;
    AFortVolume* Here = Manager ? Manager->GetVolumeForActor(reinterpret_cast<AActor*>(Pawn)) : nullptr;
    return Here ? Here : Own;
}

bool AFortPlayerControllerAthena::TeleportPawnTo(const FVector& Location, const FRotator& Rotation, const wchar_t* Where)
{
    AActor* Pawn = reinterpret_cast<AActor*>(GetPawn());
    const bool bMoved = Pawn && Pawn->TeleportTo(Location, Rotation);
    Log::Info(L"Creative: {} {} {} ({:.0f}, {:.0f}, {:.0f})", PlayerNameOf(this), bMoved ? L"goes to" : L"couldn't go to", Where, Location.X, Location.Y, Location.Z);
    return bMoved;
}

void AFortPlayerControllerAthena::ClientCycleQuickBarToCreativeItem(const FGuid& CreativeItemGuid, UFortWorldItemDefinition* CreativeItemDef)
{
    Reflection::FParams Params(FindStaticFunction_ClientCycleQuickBarToCreativeItem(), true);
    if (Params.Set(L"CreativeItemGuid", CreativeItemGuid) && Params.Set(L"CreativeItemDef", CreativeItemDef))
    {
        Params.Call(this);
    }
}

void AFortPlayerControllerAthena::ServerGiveCreativeItem_Implementation(const FFortItemEntry& CreativeItem)
{
#if WITH_SERVER_CODE
    // The Creative inventory's pick (UFortCreativeItemListMenu): the item goes into the player's inventory and their
    // quick bar moves to it (ClientCycleQuickBarToCreativeItem). Only in a Creative match.
    UFortWorldItemDefinition* Definition = Cast<UFortWorldItemDefinition>(static_cast<UFortItemDefinition*>(CreativeItem.ItemDefinition));
    AFortInventory* Inventory = WorldInventory;
    if (!Definition || !Inventory || !IsCreativeMatch(this))
    {
        Log::Warning(L"Creative: {}'s ServerGiveCreativeItem refused ({})", PlayerNameOf(this), !Definition ? L"no world item definition" : !Inventory ? L"no inventory" : L"not a Creative match");
        return;
    }
    UScriptStruct* EntryStruct = FFortItemEntry::StaticStruct();
    std::vector<uint8_t> Storage(EntryStruct->GetStructureSize(), 0);
    FFortItemEntry* Entry = reinterpret_cast<FFortItemEntry*>(Storage.data());
    EntryStruct->InitializeStruct(Entry);
    EntryStruct->CopyScriptStruct(Entry, &CreativeItem);
    const int32_t Count = std::max<int32_t>(1, Entry->Count);
    // A weapon comes loaded, as Battle Royale's loot does.
    if (Entry->LoadedAmmo <= 0)
    {
        Entry->LoadFullClip();
    }
    std::vector<UFortWorldItem*> NewItems;
    const int32_t Added = Inventory->AddWorldItemCopy(this, *Entry, Count, Count, &NewItems);
    EntryStruct->DestroyStruct(Entry);
    if (Added > 0 && !NewItems.empty() && NewItems.front())
    {
        const FFortItemEntry& NewEntry = NewItems.front()->ItemEntry;
        ClientCycleQuickBarToCreativeItem(NewEntry.ItemGuid, Definition);
    }
    Log::Info(L"Creative: {} takes {} x{} from the Creative inventory ({} added)", PlayerNameOf(this), Definition->GetName().ToStdString(), Count, Added);
#endif
}

void AFortPlayerControllerAthena::ServerReloadCreativePlot_Implementation()
{
#if WITH_SERVER_CODE
    // The island menu's reload: the player's island as it was last saved.
    AFortVolume* Volume = HasOwnedPortal() ? static_cast<AFortVolume*>(CreativePlotLinkedVolume) : nullptr;
    UFortLevelSaveComponent* SaveComponent = UFortLevelSaveComponent::GetForVolume(reinterpret_cast<AActor*>(Volume));
    const FUniqueNetIdRepl* UserId = UniqueIdOf(this);
    if (!SaveComponent || !UserId || !SaveComponent->IsOwnedBy(*UserId))
    {
        Log::Warning(L"Creative: {}'s ServerReloadCreativePlot: they have no island loaded", PlayerNameOf(this));
        return;
    }
    Log::Info(L"Creative: {} reloads their island", PlayerNameOf(this));
    SaveComponent->ReloadPlot();
#endif
}

void AFortPlayerControllerAthena::ServerStopSavingCreativePlot_Implementation()
{
#if WITH_SERVER_CODE
    // The island stops saving on its timer (ULevelSaveBaseComponent::StopPeriodicSaveTimer).
    AFortVolume* Volume = HasOwnedPortal() ? static_cast<AFortVolume*>(CreativePlotLinkedVolume) : nullptr;
    UFortLevelSaveComponent* SaveComponent = UFortLevelSaveComponent::GetForVolume(reinterpret_cast<AActor*>(Volume));
    const FUniqueNetIdRepl* UserId = UniqueIdOf(this);
    if (SaveComponent && UserId && SaveComponent->IsOwnedBy(*UserId))
    {
        SaveComponent->StopPeriodicSaveTimer();
        Log::Info(L"Creative: {}'s island stops saving", PlayerNameOf(this));
    }
#endif
}

void AFortPlayerControllerAthena::ResetMyCurrentCreativePlot_Implementation()
{
#if WITH_SERVER_CODE
    // The island menu's "Island Tools" reset: the island the player is on, when it is theirs, back to its template
    // (AFortVolumeManager::ResetVolumeAndLevelRecord, the game's own). Its next save uploads the empty record.
    AFortVolume* Volume = GetCreativeVolume();
    UFortLevelSaveComponent* SaveComponent = UFortLevelSaveComponent::GetForVolume(reinterpret_cast<AActor*>(Volume));
    const FUniqueNetIdRepl* UserId = UniqueIdOf(this);
    AFortVolumeManager* Manager = Volume ? AFortVolumeManager::Get(this) : nullptr;
    if (!Manager || !SaveComponent || !UserId || !SaveComponent->IsOwnedBy(*UserId))
    {
        Log::Warning(L"Creative: {}'s ResetMyCurrentCreativePlot refused: {}", PlayerNameOf(this), !Volume ? L"not on an island" : L"the island isn't theirs");
        return;
    }
    Manager->ResetVolumeAndLevelRecord(Volume, reinterpret_cast<AFortPlayerController*>(this));
    Log::Info(L"Creative: {} resets their island {}", PlayerNameOf(this), Volume->GetName().ToStdString());
#endif
}

void AFortPlayerControllerAthena::ServerTeleportToPlaygroundIslandDock_Implementation()
{
#if WITH_SERVER_CODE
    // To the dock of the island the player is on (its Playground.IslandDock.Spawn start), else to its own spawn.
    AFortVolume* Volume = GetCreativeVolume();
    APawn* Pawn = GetPawn();
    if (!Volume || !Pawn)
    {
        return;
    }
    const FVector From = reinterpret_cast<AActor*>(Pawn)->GetTransform().GetLocation();
    AFortPlayerStartCreative* Start = Volume->FindPlayerStart(L"Playground.IslandDock.Spawn", From);
    if (!Start)
    {
        Start = Volume->FindPlayerStart(L"Creative.Island.Spawn", From);
    }
    if (!Start)
    {
        Log::Warning(L"Creative: the island {} has no dock for {}", Volume->GetName().ToStdString(), PlayerNameOf(this));
        return;
    }
    const FTransform Transform = Start->GetTransform();
    TeleportPawnTo(Transform.GetLocation(), FRotator(0.0f, Transform.Rotation.Rotator().Yaw, 0.0f), L"the island's dock");
#endif
}

void AFortPlayerControllerAthena::ServerTeleportToPlaygroundLobbyIsland_Implementation()
{
#if WITH_SERVER_CODE
    // Back to the hub (an island's own rift, bReturnToCreativeHub, sends its pawns here): the hub's
    // Playground.LobbyIsland.Spawn start by the player's rift (its PortalIndex), else the nearest to it.
    AFortAthenaCreativePortal* Portal = GetOwnedPortal();
    APawn* Pawn = GetPawn();
    if (!Pawn)
    {
        return;
    }
    const FVector Near = Portal ? Portal->GetTransform().GetLocation() : reinterpret_cast<AActor*>(Pawn)->GetTransform().GetLocation();
    const int32_t PortalIndex = Portal ? static_cast<int32_t>(Portal->PortalIndex) : -1;
    AFortPlayerStartCreative* Best = nullptr;
    float BestDistance = std::numeric_limits<float>::max();
    UClass* StartClass = AFortPlayerStartCreative::StaticClass();
    for (AActor* Actor : StartClass ? UGameplayStatics::GetAllActorsOfClass(this, StartClass) : TArray<AActor*>())
    {
        AFortPlayerStartCreative* Start = static_cast<AFortPlayerStartCreative*>(Actor);
        if (!Start || !Start->HasTag(L"Playground.LobbyIsland.Spawn"))
        {
            continue;
        }
        const float Distance = Start->PortalIndex == PortalIndex && PortalIndex >= 0 ? -1.0f : (Start->GetTransform().GetLocation() - Near).SizeSquared();
        if (Distance < BestDistance)
        {
            BestDistance = Distance;
            Best = Start;
        }
    }
    if (Best)
    {
        const FTransform Transform = Best->GetTransform();
        TeleportPawnTo(Transform.GetLocation(), FRotator(0.0f, Transform.Rotation.Rotator().Yaw, 0.0f), L"the Creative hub");
        return;
    }
    // A hub without the tagged starts: the rift's own way to the hub (BGA_IslandPortal's TeleportToLobbyIsland: over the
    // rift, at the hub's StarterIslandTeleportHeight).
    UFunction* TeleportToLobby = Portal ? reinterpret_cast<UObject*>(Portal)->FindFunction(FName(L"TeleportToLobbyIsland", FNAME_Find)) : nullptr;
    Reflection::FParams Params(TeleportToLobby, true);
    if (TeleportToLobby && Params.Set(L"RequestingController", static_cast<AFortPlayerController*>(this)) && Params.Call(Portal))
    {
        Log::Info(L"Creative: {} goes to the Creative hub through their rift's TeleportToLobbyIsland", PlayerNameOf(this));
        return;
    }
    Log::Warning(L"Creative: {} can't go back to the hub: no Playground.LobbyIsland.Spawn start and no rift", PlayerNameOf(this));
#endif
}

void AFortPlayerControllerAthena::ServerGiveCreativeItem_ImplementationHK(AFortPlayerControllerAthena* This, const FFortItemEntry& CreativeItem)
{
    This->ServerGiveCreativeItem_Implementation(CreativeItem);
}

void AFortPlayerControllerAthena::ServerReloadCreativePlot_ImplementationHK(AFortPlayerControllerAthena* This)
{
    This->ServerReloadCreativePlot_Implementation();
}

void AFortPlayerControllerAthena::ServerStopSavingCreativePlot_ImplementationHK(AFortPlayerControllerAthena* This)
{
    This->ServerStopSavingCreativePlot_Implementation();
}

void AFortPlayerControllerAthena::ResetMyCurrentCreativePlot_ImplementationHK(AFortPlayerControllerAthena* This)
{
    This->ResetMyCurrentCreativePlot_Implementation();
}

void AFortPlayerControllerAthena::ServerTeleportToPlaygroundIslandDock_ImplementationHK(AFortPlayerControllerAthena* This)
{
    This->ServerTeleportToPlaygroundIslandDock_Implementation();
}

void AFortPlayerControllerAthena::ServerTeleportToPlaygroundLobbyIsland_ImplementationHK(AFortPlayerControllerAthena* This)
{
    This->ServerTeleportToPlaygroundLobbyIsland_Implementation();
}

void AFortPlayerControllerAthena::HookCreativeVTables()
{
#if WITH_SERVER_CODE
    // 7.00's Creative (Playlist_PlaygroundV2): the new RPCs' _Implementations are the shared empty function. Older
    // builds have no Creative rift class, and nothing here runs.
    if (!AFortAthenaCreativePortal::StaticClass() || !FindStaticFunction_ServerGiveCreativeItem())
    {
        return;
    }
    RestoreImplementation(FindStaticFunction_ServerGiveCreativeItem(), reinterpret_cast<void*>(&ServerGiveCreativeItem_ImplementationHK), L"ServerGiveCreativeItem");
    RestoreImplementation(FindStaticFunction_ServerReloadCreativePlot(), reinterpret_cast<void*>(&ServerReloadCreativePlot_ImplementationHK), L"ServerReloadCreativePlot");
    RestoreImplementation(FindStaticFunction_ServerStopSavingCreativePlot(), reinterpret_cast<void*>(&ServerStopSavingCreativePlot_ImplementationHK), L"ServerStopSavingCreativePlot");
    RestoreImplementation(FindStaticFunction_ResetMyCurrentCreativePlot(), reinterpret_cast<void*>(&ResetMyCurrentCreativePlot_ImplementationHK), L"ResetMyCurrentCreativePlot");
    RestoreImplementation(FindStaticFunction_ServerTeleportToPlaygroundIslandDock(), reinterpret_cast<void*>(&ServerTeleportToPlaygroundIslandDock_ImplementationHK),
        L"ServerTeleportToPlaygroundIslandDock");
    RestoreImplementation(FindStaticFunction_ServerTeleportToPlaygroundLobbyIsland(), reinterpret_cast<void*>(&ServerTeleportToPlaygroundLobbyIsland_ImplementationHK),
        L"ServerTeleportToPlaygroundLobbyIsland");

    // ServerLoadPlotForPortal_Implementation (7.00 0xC65E80) is the game's, but for the rift's half of the load: once it
    // has the player's id (0x1285700) it calls the shared empty function with the rift, the id and the plot item
    // (0xC65FEA), then reads the plot item's id through its vtable (call [rax+418h]) to send SetLastUsedCreativePlot.
    UFunction* LoadForPortal = FindStaticFunction_ServerLoadPlotForPortal();
    const int32_t LoadIndex = LoadForPortal ? Finder::ImplementationIndex(LoadForPortal) : -1;
    UObject* Default = LoadIndex >= 0 ? StaticClass()->GetDefaultObject(false) : nullptr;
    const uintptr_t LoadImplementation = Default ? reinterpret_cast<uintptr_t>((*reinterpret_cast<void***>(Default))[LoadIndex]) : 0;
    uint8_t* StrippedCall = nullptr;
    for (const auto& [Site, Target] : LoadImplementation && !Finder::IsStubFunction(LoadImplementation) ? Finder::DecodedDirectCalls(LoadImplementation) : std::vector<std::pair<uintptr_t, uintptr_t>>())
    {
        if (Finder::IsStubFunction(Target))
        {
            StrippedCall = reinterpret_cast<uint8_t*>(Site);
            break;
        }
    }
    if (StrippedCall)
    {
        Natives::AFortPlayerControllerAthena_ServerLoadPlotForPortal = reinterpret_cast<decltype(Natives::AFortPlayerControllerAthena_ServerLoadPlotForPortal)>(LoadImplementation);
        // The id getter: the first `call qword ptr [rax+disp32]` after the stripped call.
        for (const uint8_t* Code = StrippedCall + 5; Code < StrippedCall + 0x30 && Finder::IsReadable(Code, 6); ++Code)
        {
            if (Code[0] == 0xFF && Code[1] == 0x90)
            {
                const int32_t Displacement = *reinterpret_cast<const int32_t*>(Code + 2);
                Natives::UMcpItem_GetItemIdIndex = Displacement > 0 && Displacement % 8 == 0 ? Displacement / 8 : -1;
                break;
            }
        }
        Hooks::RedirectCall(L"AFortPlayerControllerAthena::ServerLoadPlotForPortal_Implementation's AFortAthenaCreativePortal::LoadPlotForUser", StrippedCall,
            reinterpret_cast<void*>(&AFortAthenaCreativePortal::LoadPlotForUserHK));
        Log::Info(L"Finder: AFortPlayerControllerAthena::ServerLoadPlotForPortal_Implementation at 0x{:X}, its stripped rift call at 0x{:X}, the plot item's id getter slot 0x{:X}",
            LoadImplementation - Finder::ImageBase(), reinterpret_cast<uintptr_t>(StrippedCall) - Finder::ImageBase(), Natives::UMcpItem_GetItemIdIndex);
    }
    else
    {
        Log::Error(L"Finder: AFortPlayerControllerAthena::ServerLoadPlotForPortal_Implementation's stripped rift call not found, so no island loads");
    }

    // The island's record from its file's bytes: ULevelSaveRecord::ReadFromBuffer, the call ReadFromDisk (the function
    // that logs "ULevelSaveRecord::ReadFromDisk Invalid File") makes whose own calls include ReadFromArchive (the one
    // that logs "ULevelSaveRecord::ReadFromArchive Failed to decompress data").
    const uintptr_t ReadFromDisk = FunctionReferencing(L"ULevelSaveRecord::ReadFromDisk Invalid File");
    const uintptr_t ReadFromArchive = FunctionReferencing(L"ULevelSaveRecord::ReadFromArchive Failed to decompress data");
    for (const auto& [Site, Target] : ReadFromDisk && ReadFromArchive ? Finder::DecodedDirectCalls(ReadFromDisk) : std::vector<std::pair<uintptr_t, uintptr_t>>())
    {
        const std::vector<std::pair<uintptr_t, uintptr_t>> Inner = Finder::DecodedDirectCalls(Target);
        if (std::any_of(Inner.begin(), Inner.end(), [ReadFromArchive](const auto& Call) { return Call.second == ReadFromArchive; }))
        {
            Natives::ULevelSaveRecord_ReadFromBuffer = reinterpret_cast<decltype(Natives::ULevelSaveRecord_ReadFromBuffer)>(Target);
            break;
        }
    }

    // The island's cloud save (its upload, or with bCheckin its checkin) and the sidecar's meta setter, which the save
    // calls once for each of its four meta keys and which is the one of them that formats the file's "%s/%s" key.
    const uintptr_t CloudSave = FunctionReferencing(L"Unable to start cloud save. Authority=%s, Busy=%s");
    if (CloudSave)
    {
        Natives::UFortLevelSaveComponent_CloudSave = reinterpret_cast<decltype(Natives::UFortLevelSaveComponent_CloudSave)>(CloudSave);
        // 7.00 (0xF5ADB0) takes (Plot, UserId, bCheckin) and keeps bCheckin from r9b (movzx edi, r9b); 7.10 (CL-4667333,
        // 0xF88760) reads the plot and its owner from the component and takes bCheckin second (movzx r13d, dl). The first
        // movzx of an argument byte in its prologue tells which.
        const auto* Code = reinterpret_cast<const uint8_t*>(CloudSave);
        for (int32_t At = 1; At + 3 <= 0x40; ++At)
        {
            if (Code[At] != 0x0F || Code[At + 1] != 0xB6 || (Code[At + 2] & 0xC0) != 0xC0)
            {
                continue;
            }
            const uint8_t Rex = (Code[At - 1] & 0xF0) == 0x40 ? Code[At - 1] : 0;
            const int32_t Source = (Code[At + 2] & 0x07) | ((Rex & 0x01) << 3);
            if (Source == 2 || Source == 9)
            {
                Natives::UFortLevelSaveComponent_CloudSaveTakesCheckinOnly = Source == 2;
                break;
            }
        }
        std::set<uintptr_t> KeyFormatters;
        for (const uintptr_t Ref : Finder::StringRefs(L"%s/%s"))
        {
            KeyFormatters.insert(Finder::FunctionStart(Ref));
        }
        std::map<uintptr_t, int32_t> Counts;
        for (const auto& [Site, Target] : Finder::DecodedDirectCalls(CloudSave))
        {
            ++Counts[Target];
        }
        for (const auto& [Target, Count] : Counts)
        {
            if (Count >= 4 && KeyFormatters.contains(Target))
            {
                Natives::USidecarSys_SetMeta = reinterpret_cast<decltype(Natives::USidecarSys_SetMeta)>(Target);
                break;
            }
        }
    }
    // The creative profile, which LoadCreativePlots reads (McpProfileSys, intact). UMcpProfile::GetStat: the second call
    // after the creative profile's plot permissions getter (0x1132C70) names "creative_plot_permissions" (the first is
    // the name's copy into its FString), the one whose answer's EJson type it checks next (cmp dword ptr [reg+8], 6).
    for (const uintptr_t Ref : Finder::StringRefs(L"creative_plot_permissions"))
    {
        const uintptr_t Function = Finder::FunctionStart(Ref);
        int32_t CallsAfter = 0;
        for (const auto& [Site, Target] : Function ? Finder::DecodedDirectCalls(Function) : std::vector<std::pair<uintptr_t, uintptr_t>>())
        {
            if (Site <= Ref || ++CallsAfter < 2)
            {
                continue;
            }
            for (const uint8_t* Code = reinterpret_cast<const uint8_t*>(Site + 5); Code < reinterpret_cast<const uint8_t*>(Site + 0x30) && Finder::IsReadable(Code, 4); ++Code)
            {
                if (Code[0] == 0x83 && (Code[1] & 0xF8) == 0x78 && (Code[1] & 7) != 4 && Code[2] == 0x08 && Code[3] == 0x06)
                {
                    Natives::UMcpProfile_GetStat = reinterpret_cast<decltype(Natives::UMcpProfile_GetStat)>(Target);
                    break;
                }
            }
            break;
        }
        if (Natives::UMcpProfile_GetStat)
        {
            break;
        }
    }
    // UMcpProfile's ItemsUpdatedDelegate: the member HandleProfileUpdate (0x955BE0, the function that logs its lock
    // expiration) broadcasts last, after the changed items' own delegates (lea rcx, [r15+0F8h]; call 0x2E4F40), inside
    // the profile's reflected size.
    const uintptr_t HandleProfileUpdate = FunctionReferencing(L"UMcpProfile::HandleProfileUpdate updated lock expiration on Account %s for ProfileId %s to %s");
    const std::vector<std::pair<ptrdiff_t, uintptr_t>> ProfileMemberCalls = HandleProfileUpdate ? Finder::MemberThisCalls(HandleProfileUpdate) : std::vector<std::pair<ptrdiff_t, uintptr_t>>();
    UClass* ProfileClass = UMcpProfile::StaticClass();
    const ptrdiff_t ProfileSize = ProfileClass ? ProfileClass->GetPropertiesSize() : 0;
    if (!ProfileMemberCalls.empty() && ProfileMemberCalls.back().first >= 0x28 && ProfileMemberCalls.back().first + 0x10 <= ProfileSize)
    {
        Natives::UMcpProfile_ItemsUpdatedDelegate = ProfileMemberCalls.back().first;
    }
    Log::Info(L"Finder: Creative's profile: UMcpProfile::GetStat 0x{:X}, UMcpProfile::ItemsUpdatedDelegate +0x{:X} (HandleProfileUpdate 0x{:X}, {} member call(s))",
        Natives::UMcpProfile_GetStat ? reinterpret_cast<uintptr_t>(Natives::UMcpProfile_GetStat) - Finder::ImageBase() : 0, Natives::UMcpProfile_ItemsUpdatedDelegate,
        HandleProfileUpdate ? HandleProfileUpdate - Finder::ImageBase() : 0, ProfileMemberCalls.size());
    if (!Natives::UMcpProfile_GetStat || Natives::UMcpProfile_ItemsUpdatedDelegate <= 0)
    {
        Log::Error(L"Finder: Creative's profile reader is missing a part, so players' last used islands or their late profiles aren't read (see the line above)");
    }

    const auto Address = [](const void* Function) { return Function ? reinterpret_cast<uintptr_t>(Function) - Finder::ImageBase() : 0; };
    Log::Info(L"Finder: Creative's island record: ULevelSaveRecord::ReadFromBuffer 0x{:X}, UFortLevelSaveComponent's cloud save 0x{:X} ({}), USidecarSys's meta setter 0x{:X}",
        Address(reinterpret_cast<const void*>(Natives::ULevelSaveRecord_ReadFromBuffer)), Address(reinterpret_cast<const void*>(Natives::UFortLevelSaveComponent_CloudSave)),
        Natives::UFortLevelSaveComponent_CloudSaveTakesCheckinOnly ? L"takes bCheckin only" : L"takes the plot, its owner and bCheckin",
        Address(reinterpret_cast<const void*>(Natives::USidecarSys_SetMeta)));
    if (!Natives::ULevelSaveRecord_ReadFromBuffer || !Natives::UFortLevelSaveComponent_CloudSave || !Natives::USidecarSys_SetMeta)
    {
        Log::Error(L"Finder: Creative's island record is missing a native, so islands load or save incompletely (see the line above)");
    }
#endif
}
