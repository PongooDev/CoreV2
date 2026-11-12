#include "pch.h"
#include "Athena/FortGameModeAthena.h"
#include "Athena/AI/CoreAthenaBotManager.h"
#include "Athena/FortAthenaAircraft.h"
#include "Athena/FortAthenaMapInfo.h"
#include "Athena/FortAthenaPlaylistBase.h"
#include "Athena/FortPlaylistAthena.h"
#include "Athena/FortSafeZoneIndicator.h"
#include "Building/BGAConsumableSpawner.h"
#include "Building/BuildingContainer.h"
#include "Items/FortInventory.h"
#include "Items/FortItemDefinition.h"
#include "Items/FortLootPackage.h"
#include "Pawns/FortPawn.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Athena/FortGameSessionDedicatedAthena.h"
#include "Athena/FortGameStateAthena.h"
#include "Athena/FortPlayerControllerAthena.h"
#include "Athena/FortPlayerStateAthena.h"
#include "BackendRequest.h"
#include "Building/BuildingSMActor.h"
#include "Configuration.h"
#include "CoreGlobals.h"
#include "Engine/DataTable.h"
#include "Engine/EngineTypes.h"
#include "Engine/NetConnection.h"
#include "Engine/NetDriver.h"
#include "Engine/World.h"
#include "GameFramework/Controller.h"
#include "GameFramework/GameSession.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerState.h"
#include "GameThread.h"
#include "HAL/IConsoleManager.h"
#include "Math/Rotator.h"
#include "Math/UnrealMathUtility.h"
#include "Math/Transform.h"
#include "Misc/Guid.h"
#include "Finder.h"
#include "FortZoneTheme.h"
#include "Hooks.h"
#include "Log.h"
#include "Natives.h"
#include "ObjectLayout.h"
#include "Online/NotForLicensees/FortMcpConfig.h"
#include "Online/FortGameSession.h"
#include "Online/FortPartyBeaconHost.h"
#include "Player/FortCheatManager.h"
#include "ReflectionParams.h"
#include "Templates/Casts.h"
#include "TimerManager.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/Class.h"
#include "UObject/ScriptDelegates.h"
#include "UObject/Stack.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"
#include "UObject/WeakObjectPtr.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <format>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

FString AFortGameModeAthena::DefaultMatchURL()
{
    // BP_ZT_Athena: the zone theme of Battle Royale's map (ZoneToUse /Game/Athena/Maps/Athena_Terrain, ZoneGameMode
    // Athena_GameMode_C, PlaylistId 2 solo; Core-Docs Athena-Boot-1.7.2.md §1 D5). The match's own playlist comes with
    // its matchmaking assignment. Builds whose Battle Royale map is named elsewhere (Chapter 2's) need their own source.
    // The engine asks for its default map many times while it starts.
    static FString Cached;
    if (Cached.Len() > 0)
    {
        return Cached;
    }

    constexpr const wchar_t* ThemeClassPath = L"/Game/World/ZoneThemes/PvP/BP_ZT_Athena.BP_ZT_Athena_C";
    UClass* ThemeClass = LoadObject<UClass>(nullptr, ThemeClassPath);
    UFortZoneTheme* Theme = ThemeClass ? Cast<UFortZoneTheme>(ThemeClass->GetDefaultObject()) : nullptr;
    const TAssetPtr<UWorld>* Zone = Theme ? &static_cast<TAssetPtr<UWorld>&>(Theme->ZoneToUse) : nullptr;
    const FString Map = Zone ? Zone->ToStringReference().GetLongPackageName() : FString();
    if (Map.Len() == 0)
    {
        Log::Error(L"Battle Royale: no map from {}, the server starts on its default map", ThemeClassPath);
        return FString();
    }

    const std::wstring URL = std::format(L"{}?playlistId={}", Map.ToStdString(), static_cast<int32_t>(Theme->PlaylistId));
    Log::Info(L"Battle Royale: the server starts on {}", URL);
    Cached = FString(URL.c_str());
    return Cached;
}

AFortGameModeAthena* AFortGameModeAthena::GetCurrent(const UObject* WorldContextObject)
{
    UWorld* World = WorldContextObject ? WorldContextObject->GetWorld() : nullptr;
    return World ? Cast<AFortGameModeAthena>(reinterpret_cast<UObject*>(World->GetAuthGameMode())) : nullptr;
}

TSubclassOf<AGameSession> AFortGameModeAthena::GetGameSessionClass() const
{
    // Core-Backend #301: with 5.0's base AFortGameSessionDedicated the session was created but never registered with the
    // matchmaking service, so no ticket reached a server.
    if (IsRunningDedicatedServer())
    {
        if (UClass* SessionClass = AFortGameSessionDedicatedAthena::StaticClass())
        {
            return SessionClass;
        }
    }

    return AFortGameModeZone::GetGameSessionClass();
}

namespace
{
    AFortAthenaMapInfo* FindMapInfo(UWorld* World)
    {
        AFortGameStateAthena* GameState = World ? Cast<AFortGameStateAthena>(reinterpret_cast<UObject*>(World->GetGameState())) : nullptr;
        if (GameState && static_cast<AFortAthenaMapInfo*>(GameState->MapInfo))
        {
            return GameState->MapInfo;
        }
        // Core runs before AFortGameModeAthena's own FinishWorldInitialization, which sets MapInfo: take the placed one.
        for (TObjectIterator<AFortAthenaMapInfo> It; It; ++It)
        {
            AFortAthenaMapInfo* MapInfo = *It;
            if (MapInfo->GetWorld() == World && !MapInfo->HasAnyFlags(static_cast<EObjectFlags>(RF_ClassDefaultObject | RF_ArchetypeObject)))
            {
                return MapInfo;
            }
        }
        return nullptr;
    }

    // Keeps a share of the world's placed ContainerClass between the two spawn percents, picked at random once a match,
    // and destroys the rest, picked at random. The rows' units differ: AthenaGameData's Default.TreasureChest rows are
    // percents (50 and 70) and its Default.AmmoBox rows fractions (0.65 and 0.8), so a value over 1 is read as a percent.
    void ApplySpawnPercent(UWorld* World, UClass* ContainerClass, const FScalableFloat& MinSpawnPercent, const FScalableFloat& MaxSpawnPercent, const wchar_t* What)
    {
        if (!ContainerClass)
        {
            Log::Info(L"Battle Royale: the map info has no {} class, every placed one spawns", What);
            return;
        }
        const auto ToFraction = [](float Percent) { return FMath::Clamp(Percent > 1.0f ? Percent / 100.0f : Percent, 0.0f, 1.0f); };
        const float MinFraction = ToFraction(MinSpawnPercent.GetValueAtLevel(0.0f));
        const float MaxFraction = ToFraction(MaxSpawnPercent.GetValueAtLevel(0.0f));
        const float SpawnFraction = MinFraction + (std::max(MaxFraction, MinFraction) - MinFraction) * FMath::FRand();

        std::vector<ABuildingContainer*> Containers;
        for (TObjectIterator<ABuildingContainer> It; It; ++It)
        {
            ABuildingContainer* Container = *It;
            if (Container->GetWorld() == World && !Container->HasAnyFlags(static_cast<EObjectFlags>(RF_ClassDefaultObject | RF_ArchetypeObject)) && Container->IsA(ContainerClass) && !Container->bDestroyed)
            {
                Containers.push_back(Container);
            }
        }
        const int32_t Placed = static_cast<int32_t>(Containers.size());
        const int32_t Kept = static_cast<int32_t>(std::lround(static_cast<float>(Placed) * SpawnFraction));
        for (int32_t Index = Placed - 1; Index > 0; --Index)
        {
            std::swap(Containers[Index], Containers[FMath::Rand() % (Index + 1)]);
        }
        // ABuildingActor's K2_DestroyActor override kills the building (bDestroyed, hidden; it replicates) rather than
        // destroying the actor. It has no loot tier from then on, as a container that rolled no loot.
        int32_t Destroyed = 0;
        for (int32_t Index = Kept; Index < Placed; ++Index)
        {
            ABuildingContainer* Container = Containers[Index];
            Container->K2_DestroyActor();
            if (Container->bDestroyed)
            {
                Container->ContainerLootTierKey = FName();
                ++Destroyed;
            }
        }
        Log::Info(L"Battle Royale: {} of {} {} spawn ({:.0f}%, spawn percents {:.0f}-{:.0f}%), {} destroyed", Placed - Destroyed, Placed, What, SpawnFraction * 100.0f, MinFraction * 100.0f, MaxFraction * 100.0f, Destroyed);
    }

    void ApplySpawnPercents(AFortGameModeAthena* GameMode)
    {
        UWorld* World = GameMode->GetWorld();
        AFortAthenaMapInfo* MapInfo = AFortAthenaMapInfo::StaticClass() ? FindMapInfo(World) : nullptr;
        if (!MapInfo)
        {
            return;
        }
        Log::Info(L"Battle Royale: the chests' and ammo boxes' spawn percents are playlist {}'s", static_cast<int32_t>(GameMode->CurrentPlaylistId));
        if (AFortAthenaMapInfo::HasTreasureChestSpawnPercent())
        {
            ApplySpawnPercent(World, MapInfo->TreasureChestClass, MapInfo->TreasureChestMinSpawnPercent, MapInfo->TreasureChestMaxSpawnPercent, L"treasure chests");
        }
        if (AFortAthenaMapInfo::HasAmmoBoxSpawnPercent())
        {
            ApplySpawnPercent(World, MapInfo->AmmoBoxClass, MapInfo->AmmoBoxMinSpawnPercent, MapInfo->AmmoBoxMaxSpawnPercent, L"ammo boxes");
        }
    }

    AFortGameStateAthena* AthenaGameStateOf(AFortGameModeAthena* GameMode)
    {
        UWorld* World = GameMode->GetWorld();
        return World ? Cast<AFortGameStateAthena>(reinterpret_cast<UObject*>(World->GetGameState())) : nullptr;
    }

    // Core's record of this world's match: the work that waits for one of the game's events, and the timers Core set on
    // the world's timer manager. Each timer is a one-off that sets itself again while it is needed, so it stops with the
    // game mode (the game instance's timer manager outlives the world). A new game mode starts it over.
    struct FMatchEvents
    {
        FWeakObjectPtr GameMode;
        // The game phase HandleGamePhaseChanged last handled.
        uint8_t HandledPhase = 0xFF;
        // Whether the game state's GamePhaseChanged calls HandleGamePhaseChanged, and the pawns' OnDied HandlePawnDied.
        bool bPhaseBound = false;
        bool bPawnsBound = false;
        // ApplyContainerSpawnPercents left the spawn percents to the bus.
        bool bSpawnPercentsWaitForAircraft = false;
        bool bAircraftHandled = false;
        // The tick after GamePhaseChanged (RunGamePhase); where it can't be bound, CheckGamePhase's.
        FTimerHandle PhaseTimer;
        // The tick after a pawn of the match died or a player left (CheckMatchPlayers).
        FTimerHandle PlayersTimer;
        // The tick after OnCurrentPlaylistLoaded; before 4.4, CheckMatchPlaylist's.
        FTimerHandle PlaylistTimer;
    };

    // A delegate's work runs the tick after the game's broadcast, outside the game function that broadcast it (the
    // game mode's StartAircraftPhase, a pawn's death, the game state's OnPlaylistDataLoadCompleted), as Core's timers do.
    constexpr float NextTickDelay = 0.001f;
    FMatchEvents MatchEvents;

    // Core's natives on the Battle Royale game mode's class, which the game's delegates call
    // (AFortGameModeAthena::AddDelegateHandlers, at boot); null where the build has no such delegate or Core couldn't
    // add one.
    UFunction* GamePhaseChangedHandler = nullptr;
    UFunction* GamePhaseStepChangedHandler = nullptr;
    UFunction* SafeZoneUpdatedHandler = nullptr;
    UFunction* CurrentPlaylistLoadedHandler = nullptr;
    UFunction* PawnDiedHandler = nullptr;
    UFunction* PlayerStateDestroyedHandler = nullptr;
    UFunction* PawnDestroyedHandler = nullptr;

    FMatchEvents& MatchEventsOf(const AFortGameModeAthena* GameMode)
    {
        if (MatchEvents.GameMode.Get() != GameMode)
        {
            MatchEvents = FMatchEvents();
            MatchEvents.GameMode = FWeakObjectPtr(GameMode);
        }
        return MatchEvents;
    }

    // A one-off timer on the game mode's world (its game instance's timer manager): Then runs Delay seconds from now, as
    // long as the game mode is still there. A timer already in Handle is replaced.
    void SetGameModeTimer(AFortGameModeAthena* GameMode, FTimerHandle& Handle, float Delay, void (*Then)(AFortGameModeAthena*))
    {
        UWorld* World = GameMode ? GameMode->GetWorld() : nullptr;
        if (!World)
        {
            return;
        }
        const FWeakObjectPtr WeakGameMode(GameMode);
        World->GetTimerManager().SetTimer(Handle, FTimerDelegate::CreateLambda([WeakGameMode, Then]()
        {
            if (AFortGameModeAthena* Athena = static_cast<AFortGameModeAthena*>(WeakGameMode.Get()))
            {
                Then(Athena);
            }
        }), Delay, false);
    }

    void ClearGameModeTimer(AFortGameModeAthena* GameMode, FTimerHandle& Handle)
    {
        UWorld* World = GameMode && Handle.IsValid() ? GameMode->GetWorld() : nullptr;
        if (World)
        {
            World->GetTimerManager().ClearTimer(Handle);
        }
        Handle.Invalidate();
    }

    // The signature of Class's Blueprint delegate PropertyName (its UMulticastDelegateProperty's SignatureFunction), or
    // null where the class has no such delegate.
    const UFunction* DelegateSignature(UClass* Class, const wchar_t* PropertyName)
    {
        const UProperty* Property = Class ? Class->FindPropertyByName(FName(PropertyName)) : nullptr;
        return Property && Property->GetClass()->HasAnyCastFlag(CASTCLASS_UMulticastDelegateProperty)
            ? static_cast<UFunction*>(static_cast<const UMulticastDelegateProperty*>(Property)->SignatureFunction) : nullptr;
    }

    // Core's: Epic's call to AFortAthenaMapInfo::SpawnLlamas went with the stripped server code. 3.3 keeps no call to it
    // (no call to the shared empty function has the map info as this), so it can't be redirected. The llamas are placed
    // when the bus is under way (HandleGamePhaseChanged), the match's playlist final by then: their quantity rows are
    // AthenaGameData's, which a playlist's own game data replaces (as for the spawn percents), and the match's playlist
    // comes with its assignment.
    void SpawnMapLlamas(AFortGameModeAthena* GameMode)
    {
        AFortGameStateAthena* GameState = AFortAthenaMapInfo::HasLlamas() ? AthenaGameStateOf(GameMode) : nullptr;
        if (AFortAthenaMapInfo* MapInfo = GameState ? static_cast<AFortAthenaMapInfo*>(GameState->MapInfo) : nullptr)
        {
            MapInfo->SpawnLlamas();
        }
    }
}

void AFortGameModeAthena::ApplyContainerSpawnPercents()
{
#if WITH_SERVER_CODE
    // AFortAthenaMapInfo's TreasureChestClass and AmmoBoxClass with their Min/MaxSpawnPercent (from 1.7.2). The code that
    // read them is server-only and no client build has it (Core-Docs Athena-Gameplay-1.7.2.md §2.4), so every placed chest
    // spawned: on Epic's servers 50 to 70% of them did (#205).
    if (FParse::Param(FCommandLine::Get(), L"SpawnAllLoot"))
    {
        return;
    }
    // The spawn percents are AthenaGameData's rows (Default.TreasureChest and Default.AmmoBox), and from 1.11 a playlist's
    // own game data replaces them: ApplyPlaylistDataTables copies its GameData over AthenaGameData ("Patched Game Data
    // with %s", 3.2 0x9AEF10, the curve table at the playlist class default +0x360), as Teams of 20's does (#238: chests
    // 60-80%, ammo boxes 75-90%). The world starts on the map URL's playlist and the match's comes with its assignment,
    // so on those builds the percents wait for the bus, when the playlist is the match's and its game data in place
    // (HandleGamePhaseChanged). Nobody is near the map's containers then (the bus flies at AthenaGameData's
    // Default.Aircraft.Height, 80000 on 3.2).
    if (Finder::ResolvePlaylistDataTablesLive())
    {
        MatchEventsOf(this).bSpawnPercentsWaitForAircraft = true;
        return;
    }
    ApplySpawnPercents(this);
#endif
}

void AFortGameModeAthena::SpawnBGAConsumables()
{
#if WITH_SERVER_CODE
    // The spawners roll from the playlist's tables (21.40's spawner waits for them: bAthenaPostApplyPlaylistDataTables).
    ApplyPlaylistDataTablesOnce();
    ABGAConsumableSpawner::SpawnWorldConsumables(GetWorld());
#endif
}

FName AFortGameModeAthena::RedirectLootTierGroup(FName TierGroup)
{
    // The override (1.7.2 0x3FA190) is intact, but its caller went with the stripped container and building code, so
    // Battle Royale's chests and ammo boxes rolled Save the World's Loot_Treasure and Loot_Ammo.
    // FName* RedirectLootTierGroup(FName* OutResult, FName InGroup), the name passed by value in r8.
    if (!Finder::ResolveLootTierRedirectLive())
    {
        return TierGroup;
    }
    FName Redirected;
    CallVirtual<FName*>(this, Natives::AFortGameMode_RedirectLootTierGroupIndex, &Redirected, TierGroup);
    return Redirected;
}

bool AFortGameModeAthena::AddPlaylistContextTag(FGameplayTagContainer& Tags)
{
    // The game mode's GameContextTags (1.8 +0x5A0) are the context of every quest event (AFortPlayerController's
    // GetSourceAndContextTags, 0x87DC20, through the game mode's 0x3F2D80), and Battle Royale's objectives ask for the
    // playlist there (interact_athena_treasurechest, daily_athena_loot_chest's: Athena.Playlist). Only Save the World's
    // world record fills them (OnInitWorldRecordComplete, 0x591340: theater, zone theme and tile tags; BP_ZT_Athena has
    // no ZoneTags), and no code names the Athena.Playlist tags (PvPTags rows 21-23, and 1.10's row 24
    // Athena.Playlist.FiftyFifty for 50v50), so a Battle Royale event had no context and matched none of them. The
    // playlist ids are MCP's, as the match's CurrentPlaylistId has them. The lobby's event queues EventN send playlists
    // 15, 13, 14, 17, 12, 16, 18, 19 for N = 1..8 (the matchmaking button's switch, 2.4.2 0x20FBC90), and their quests
    // (athenaeventN_playmatches) count Athena.Playlist.Event.N (Core-Backend #190, #195).
    const int32_t PlaylistId = CurrentPlaylistId;
    static const wchar_t* const EventNames[] = { L"Athena.Playlist.Event.5", L"Athena.Playlist.Event.2", L"Athena.Playlist.Event.3", L"Athena.Playlist.Event.1",
        L"Athena.Playlist.Event.6", L"Athena.Playlist.Event.4", L"Athena.Playlist.Event.7", L"Athena.Playlist.Event.8" };
    // 3.0's queues past AthenaEvent8 play their own playlist: the button's switch (3.0 0xD660F0) gives AthenaEvent<N>
    // playlist N from 21 to 54, so playlist N's matches are queue N's, Athena.Playlist.Event.<N>, as Playlist_5x20's
    // AthenaPlaylistTag has it (PlaylistId 22) and the quests count them (athenaevent27_playmatches:
    // complete_athena_event_27, Solid Gold's playlist 27).
    const std::wstring QueueName = PlaylistId >= 21 && PlaylistId <= 54 ? std::format(L"Athena.Playlist.Event.{}", PlaylistId) : std::wstring();
    const wchar_t* const Name = PlaylistId == 2 ? L"Athena.Playlist.Solo" : PlaylistId == 10 ? L"Athena.Playlist.Duo" : PlaylistId == 9 ? L"Athena.Playlist.Squad"
        : PlaylistId == 11 ? L"Athena.Playlist.FiftyFifty" : PlaylistId >= 12 && PlaylistId <= 19 ? EventNames[PlaylistId - 12]
        : !QueueName.empty() && !FName(QueueName.c_str(), FNAME_Find).IsNone() ? QueueName.c_str() : L"Athena.Playlist";
    const FGameplayTag Tag{ FName(Name) };
    if (Tags.GameplayTags.Contains(Tag))
    {
        return false;
    }
    Tags.GameplayTags.Add(Tag);
    for (const wchar_t* const ParentName : { L"Athena.Playlist", L"Athena" })
    {
        const FGameplayTag Parent{ FName(ParentName) };
        if (!(Parent == Tag) && !Tags.ParentTags.Contains(Parent))
        {
            Tags.ParentTags.Add(Parent);
        }
    }
    return true;
}

APawn* AFortGameModeAthena::SpawnDefaultPawnFor_Implementation(AController* NewPlayer, AActor* StartSpot)
{
#if WITH_SERVER_CODE
    // The aircraft is the start spot of everyone in it (ChoosePlayerStart, 0x3CA260), and at the end of its path everyone
    // still inside leaves it in the same frame (OnAircraftExitedDropZone, 0x3F35B0: RestartPlayer for each). AFortGameMode's
    // spawn (0x592920) won't put a pawn within 6 units of another player's: it asks ChoosePlayerStart for another spot, the
    // aircraft again, five times, then fails. So only the first of them left the bus and the rest stayed inside (#80). The
    // aircraft spawns the pawn where it is, whatever is there, as the engine's default spawn does (the spot's location and
    // yaw); the warmup island's spots keep AFortGameMode's spawn.
    AFortAthenaAircraft* Aircraft = Cast<AFortAthenaAircraft>(StartSpot);
    // A respawn in the safe zones phase (4.2's Playground, #308): ChoosePlayerStart (4.2 0xA0AE70) gives the engine's
    // player start, on the warmup island, and RespawnPlayerAfterDeath (0xA57AD0) has the new pawn BeginSkydiving there.
    // The playlist's RespawnLocation is Air, which no code left in the build reads: the pawn starts in the air over
    // where the player died, at the playlist's RespawnHeight (GetRespawnHeight; TeleportToSkyDive's height, 0xA98690,
    // where the playlist has none), and skydives from there.
    FVector RespawnLocation;
    float RespawnHeight = 0.0f;
    bool bRespawnInAir = false;
    // What the game's FinishRestartPlayer does with the pawn next, for the log.
    std::wstring GameRespawnPlace;
    if (!Aircraft && StartSpot && NewPlayer && FDeathInfo::HasDeathLocation() && AFortPlayerStateAthena::HasEliminator())
    {
        UWorld* SpotWorld = GetWorld();
        AFortGameStateAthena* GameState = SpotWorld ? Cast<AFortGameStateAthena>(reinterpret_cast<UObject*>(SpotWorld->GetGameState())) : nullptr;
        AFortPlayerStateAthena* AthenaState = Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(static_cast<APlayerState*>(NewPlayer->PlayerState)));
        // From 6.01 the death is stored as the game stores one (AFortPlayerControllerAthena::OnPawnDied), and the game's
        // own FinishRestartPlayer (6.02 0xBC7430, 6.01 0xBC5DB0) then moves the pawn spawned here: TeleportTo the respawn
        // place the death handler worked out (6.02's RespawnData; 6.01 works it out there), a mutator's (Disco
        // Domination's, by the team's side of the current circle) or else the death's location, raised by the
        // playlist's RespawnHeight where the playlist has bRespawnInAir, up to 55000. That place comes from the stored
        // DeathLocation, not from where this pawn is, so the height isn't added twice: Playground's ends where it is
        // spawned here, Disco Domination's at the mutator's place. The spawn in the air stays, as where the pawn is
        // left should that move fail (TeleportTo checks the place), and it is given whenever the game will make the
        // move, which is for a stored death (bInitialized) and not what IsRespawningAllowed says now: its storm rule
        // was asked as the player died, and says no once the storm has passed over where that was.
        const bool bStoredDeath = AthenaState && AFortPlayerStateAthena::HasSetDeathInfo() && FDeathInfo::HasInitialized() && AthenaState->DeathInfo.bInitialized && !AthenaState->DeathInfo.bDBNO;
        if (GameState && AthenaState && GameState->GamePhase == EAthenaGamePhase::GetSafeZones() && (bStoredDeath || IsRespawningAllowed(AthenaState)))
        {
            const FVector DeathLocation = AthenaState->DeathInfo.DeathLocation;
            bRespawnInAir = DeathLocation.X != 0.0f || DeathLocation.Y != 0.0f || DeathLocation.Z != 0.0f;
            RespawnHeight = GetRespawnHeight();
            RespawnLocation = DeathLocation + FVector(0.0f, 0.0f, RespawnHeight);
            if (bStoredDeath && AFortPlayerStateAthena::HasRespawnData() && AthenaState->RespawnData.bRespawnDataAvailable)
            {
                const FFortRespawnData& RespawnData = AthenaState->RespawnData;
                const FVector Place = RespawnData.RespawnLocation;
                // FinishRestartPlayer makes the move once both sides are ready (RespawnPlayerAfterDeath waits for that).
                const bool bClientIsReady = RespawnData.bClientIsReady;
                const bool bServerIsReady = RespawnData.bServerIsReady;
                GameRespawnPlace = std::format(L"; the death's respawn place is ({:.0f}, {:.0f}, {:.0f}), where the game {} (the client {} ready for it, the server {})", Place.X, Place.Y, Place.Z,
                    bClientIsReady && bServerIsReady ? L"moves it next" : L"doesn't move it", bClientIsReady ? L"is" : L"isn't", bServerIsReady ? L"is" : L"isn't");
            }
            else if (bStoredDeath)
            {
                GameRespawnPlace = L"; the game then moves it to the respawn place it works out for the stored death";
            }
        }
    }
    UClass* PawnClass = (Aircraft || bRespawnInAir) && NewPlayer ? GetDefaultPawnClassForController(NewPlayer) : nullptr;
    UWorld* World = PawnClass ? GetWorld() : nullptr;
    if (World)
    {
        const FTransform SpotTransform = Aircraft ? Aircraft->GetTransform() : StartSpot->GetTransform();
        const float HalfYaw = SpotTransform.Rotation.Rotator().Yaw * (3.1415926535897932f / 360.0f);
        FTransform SpawnTransform;
        SpawnTransform.Rotation.Z = std::sin(HalfYaw);
        SpawnTransform.Rotation.W = std::cos(HalfYaw);
        SpawnTransform.SetLocation(Aircraft ? SpotTransform.GetLocation() : RespawnLocation);

        FActorSpawnParameters SpawnInfo;
        SpawnInfo.ObjectFlags = SpawnInfo.ObjectFlags | RF_Transient;
        SpawnInfo.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        APawn* Pawn = Cast<APawn>(World->SpawnActor(PawnClass, &SpawnTransform, SpawnInfo));
        if (Pawn && !Aircraft)
        {
            // The pawn's address goes with it: HandlePawnDestroyed logs each dead pawn's (#411).
            Log::Info(L"Battle Royale: {} respawns in the air as {} (0x{:X}) at ({:.0f}, {:.0f}, {:.0f}), {:.0f} over where they died{}", NewPlayer->GetName().ToStdString(),
                Pawn->GetName().ToStdString(), reinterpret_cast<uintptr_t>(Pawn), RespawnLocation.X, RespawnLocation.Y, RespawnLocation.Z, RespawnHeight, GameRespawnPlace);
        }
        else if (Pawn)
        {
            Log::Info(L"Battle Royale: {} leaves the aircraft as {}", NewPlayer->GetName().ToStdString(), Pawn->GetName().ToStdString());
        }
        else
        {
            Log::Error(L"Battle Royale: couldn't spawn {}'s {} {}", NewPlayer->GetName().ToStdString(), PawnClass->GetName().ToStdString(), Aircraft ? L"at the aircraft" : L"in the air");
        }
        return Pawn;
    }
#endif
    return AFortGameModeZone::SpawnDefaultPawnFor_Implementation(NewPlayer, StartSpot);
}

void AFortGameModeAthena::ModifyDeathReport(AFortPlayerControllerAthena* Player, FFortPlayerDeathReport& DeathReport)
{
#if WITH_SERVER_CODE
    // 1.9.1 and 1.10 keep only the Cast<AFortGameModeAthena> of AFortPlayerControllerAthena::PrepareDeathReport's call
    // (1.10 0x449F80, 1.9.1 0x431090: a jump to AFortGameModeAthena::StaticClass after AFortPlayerControllerZone's
    // report). The report's killer is the death's instigating controller (1.10 0x8DB450), and a fall or the storm has
    // none: falling damage is the game state's environment damage (AFortPawn 0x81E980, FallingDamageTemplateGE through
    // 0x2A4840, "EnvironmentDamage"). The elimination screen takes the eliminator's banner from KillerPlayerState only
    // (EliminatedOverlay's SetBannerForKiller; its PlayerRankEmblem doesn't bind to the local player) and, with no
    // killer pawn, says "YOU ELIMINATED YOURSELF" with the player's own name (ProcessDeathReport), so it showed the
    // empty emblem (#173). A player nobody eliminated is their own eliminator, as the death handler has it for DeathInfo
    // (1.8 0x43BAFE: FinisherOrDowner is the player's own state); KillerPawn stays empty, so the death camera turns to
    // nobody and the screen still reads "YOU ELIMINATED YOURSELF".
    if (Player && !DeathReport.KillerPlayerState)
    {
        DeathReport.KillerPlayerState = Cast<AFortPlayerState>(reinterpret_cast<UObject*>(static_cast<APlayerState*>(Player->PlayerState)));
    }
#endif
}

namespace
{
    // A player of the match, kept after they leave it: a team's place comes when its last player is out, by which time
    // the others may have gone back to the lobby.
    struct FMatchPlayer
    {
        uint8_t Team = 0;
        int32_t Kills = 0;
        // Of those, the player bots they eliminated (CoreV2 #233): the backend leaves them out of the kill challenges
        // when the admin panel says bot eliminations don't count for quests.
        int32_t BotKills = 0;
        int32_t SecondsAlive = 0;
        int32_t Place = 0;
        // Who eliminated the player (their account id), on a build whose server doesn't count kills (1.8): empty while
        // the player is alive or was out by the storm or a fall.
        std::wstring EliminatedBy;
        bool bReported = false;
    };

    struct FMatchResults
    {
        FWeakObjectPtr GameMode;
        std::map<std::wstring, FMatchPlayer> Players;
        // Whether the backend should count a place 1 as the win: the build doesn't report its winners itself.
        bool bCountWins = false;
        // The match's own id, new for each match, so the backend pays its V-Bucks rewards once (Core-Backend #100).
        std::string MatchId;
    };

    // An MCP account id is 32 hex digits (an invalid id's ToString is "INVALID").
    bool IsAccountId(const std::wstring& Id)
    {
        return !Id.empty() && Id.size() <= 64 && std::all_of(Id.begin(), Id.end(), [](wchar_t Character) { return std::iswxdigit(Character) != 0; });
    }

    // The end-of-match rewards of the players just reported who are still here, from the backend's answer to the report
    // ({results: [{accountId, ...}]}, one object per player it gave something): each player's own object, or none.
    void SendMatchRewards(AFortGameModeAthena* GameMode, const std::vector<std::wstring>& Reported, const std::string& Answer)
    {
        UWorld* World = GameMode ? GameMode->GetWorld() : nullptr;
        AGameStateBase* GameState = World ? World->GetGameState() : nullptr;
        if (!GameState)
        {
            return;
        }
        const std::string AccountKey = "{\"accountId\":\"";
        for (APlayerState* PlayerState : GameState->PlayerArray)
        {
            AFortPlayerStateAthena* AthenaState = Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(PlayerState));
            const std::wstring AccountId = AthenaState ? AthenaState->UniqueId.ToString().ToStdString() : std::wstring();
            AFortPlayerController* Controller = AthenaState ? Cast<AFortPlayerController>(reinterpret_cast<UObject*>(AthenaState->GetOwner())) : nullptr;
            if (!Controller || std::find(Reported.begin(), Reported.end(), AccountId) == Reported.end())
            {
                continue;
            }
            std::string Id;
            std::transform(AccountId.begin(), AccountId.end(), std::back_inserter(Id), [](wchar_t Digit) { return static_cast<char>(Digit); });
            const size_t Start = Answer.find(AccountKey + Id + "\"");
            const size_t Next = Start == std::string::npos ? std::string::npos : Answer.find(AccountKey, Start + AccountKey.size());
            Controller->SendEndBattleRoyaleMatchForPlayer(Start == std::string::npos ? std::string() : Answer.substr(Start, Next == std::string::npos ? std::string::npos : Next - Start));
        }
    }

    // A player of the match as their player state has them now (none for a bot, which has no account): their team,
    // time alive, place and, where the server doesn't count kills, who eliminated them.
    FMatchPlayer* UpdateMatchPlayer(FMatchResults& Results, AFortPlayerStateAthena* AthenaState)
    {
        const std::wstring AccountId = AthenaState ? AthenaState->UniqueId.ToString().ToStdString() : std::wstring();
        if (!IsAccountId(AccountId))
        {
            return nullptr;
        }
        FMatchPlayer& Player = Results.Players[AccountId];
        if (Player.bReported)
        {
            return nullptr;
        }
        Player.Team = AthenaState->TeamIndex;
        Player.SecondsAlive = AthenaState->SecondsAlive;
        Player.Place = std::max<int32_t>(AthenaState->Place, 0);
        if (!AFortPlayerStateAthena::CountsKills() && Player.EliminatedBy.empty())
        {
            AFortPlayerStateAthena* Eliminator = AthenaState->GetEliminator();
            const std::wstring EliminatorId = Eliminator && Eliminator != AthenaState ? Eliminator->UniqueId.ToString().ToStdString() : std::wstring();
            if (IsAccountId(EliminatorId))
            {
                Player.EliminatedBy = EliminatorId;
            }
        }
        return &Player;
    }

    // Brings the players up to date from their player states and reports those whose team has a place.
    void CheckMatchResults(AFortGameModeAthena* GameMode, FMatchResults& Results)
    {
        UWorld* World = GameMode->GetWorld();
        AGameStateBase* GameState = World ? World->GetGameState() : nullptr;
        if (!GameState)
        {
            return;
        }

        const TArray<APlayerState*>& PlayerArray = GameState->PlayerArray;
        for (APlayerState* PlayerState : PlayerArray)
        {
            UpdateMatchPlayer(Results, Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(PlayerState)));
        }

        // The kills, final by the time the player's team has a place. On 1.8 they are the other teams' players the
        // player eliminated, as 1.7.2's death handler counts them, kept in KillScore for the game's own use 🟡 (the HUD
        // and scoreboard weren't looked at).
        for (APlayerState* PlayerState : PlayerArray)
        {
            AFortPlayerStateAthena* AthenaState = Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(PlayerState));
            const std::wstring AccountId = AthenaState ? AthenaState->UniqueId.ToString().ToStdString() : std::wstring();
            const auto Found = IsAccountId(AccountId) ? Results.Players.find(AccountId) : Results.Players.end();
            if (Found == Results.Players.end() || Found->second.bReported)
            {
                continue;
            }
            FMatchPlayer& Player = Found->second;
            if (!AFortPlayerStateAthena::CountsKills())
            {
                const int32_t Kills = static_cast<int32_t>(std::count_if(Results.Players.begin(), Results.Players.end(), [&](const auto& Other)
                {
                    return Other.second.EliminatedBy == AccountId && Other.second.Team != Player.Team;
                }));
                // With the kills the player's "cheat killscore" added (Core-Backend #145), and the player bots they
                // eliminated (CoreV2 #233; bots have no account, so they aren't among the match's players here).
                AthenaState->SetKillScore(std::max(Kills + UFortCheatManager::KillScoreOffset(AthenaState) + FCoreAthenaBotManager::BotKillsOf(AthenaState), 0));
            }
            Player.Kills = AthenaState->GetKills();
            Player.BotKills = std::min(FCoreAthenaBotManager::BotKillsOf(AthenaState), std::max(Player.Kills, 0));
        }

        // The team's place, for its players who left before it was out.
        for (auto& [AccountId, Player] : Results.Players)
        {
            for (const auto& [OtherId, Other] : Results.Players)
            {
                if (Player.Place == 0 && Other.Place > 0 && Other.Team == Player.Team)
                {
                    Player.Place = Other.Place;
                }
            }
        }

        std::string Body;
        int32_t Count = 0;
        std::vector<std::wstring> Reported;
        for (auto& [AccountId, Player] : Results.Players)
        {
            if (Player.bReported || Player.Place == 0)
            {
                continue;
            }
            Player.bReported = true;
            Reported.push_back(AccountId);
            std::string Id;
            std::transform(AccountId.begin(), AccountId.end(), std::back_inserter(Id), [](wchar_t Digit) { return static_cast<char>(Digit); });
            Body += std::format("{}{{\"accountId\":\"{}\",\"place\":{},\"kills\":{},\"botKills\":{},\"secondsAlive\":{}}}", Count++ > 0 ? "," : "",
                Id, Player.Place, std::max(Player.Kills, 0), Player.BotKills, std::max(Player.SecondsAlive, 0));
            Log::Info(L"Battle Royale: {} placed {} with {} kills, {} s alive", AccountId, Player.Place, Player.Kills, Player.SecondsAlive);
        }

        // The end-of-match screen of each player still here whose place is final, its kills counted above: their stats
        // if they haven't gone yet (a player out of the match had theirs as they went out, from 1.11) and their team's
        // place (AFortPlayerController::SendMatchStatsForPlayer sends each once).
        for (APlayerState* PlayerState : PlayerArray)
        {
            AFortPlayerStateAthena* AthenaState = Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(PlayerState));
            const std::wstring AccountId = AthenaState ? AthenaState->UniqueId.ToString().ToStdString() : std::wstring();
            const auto Found = IsAccountId(AccountId) ? Results.Players.find(AccountId) : Results.Players.end();
            if (Found != Results.Players.end() && Found->second.bReported)
            {
                if (AFortPlayerController* Controller = Cast<AFortPlayerController>(reinterpret_cast<UObject*>(AthenaState->GetOwner())))
                {
                    Controller->SendMatchStatsForPlayer();
                }
            }
        }
        if (Count > 0)
        {
            const int32_t PlaylistId = GameMode->CurrentPlaylistId;
            const FWeakObjectPtr WeakGameMode(GameMode);
            BackendRequest::PostJson(std::format(L"Battle Royale results of {} players", Count), L"/fortnite/api/game/v2/battleroyale/recordresults",
                std::format("{{\"playlistId\":{},\"results\":[{}],\"countWins\":{},\"matchId\":\"{}\"}}", PlaylistId, Body,
                    Results.bCountWins ? "true" : "false", Results.MatchId),
                FFortMcpConfig::DedicatedServerClientId, FFortMcpConfig::DedicatedServerClientSecret, [WeakGameMode, Reported](uint32_t Status, const std::string& Answer)
            {
                GameThread::Enqueue([WeakGameMode, Reported, Answer = Status == 200 ? Answer : std::string()]()
                {
                    SendMatchRewards(static_cast<AFortGameModeAthena*>(WeakGameMode.Get()), Reported, Answer);
                });
            });
        }
    }

    // This world's match results (StartMatchResults).
    std::shared_ptr<FMatchResults> MatchResults;

    FMatchResults* MatchResultsOf(const AFortGameModeAthena* GameMode)
    {
        return MatchResults && MatchResults->GameMode.Get() == GameMode ? MatchResults.get() : nullptr;
    }
}

void AFortGameModeAthena::StartMatchResults()
{
#if WITH_SERVER_CODE
    // Epic's stats service kept Battle Royale's stats (Balance/DataTables/LeaderboardStats: br_placetop<n>, br_kills,
    // br_matchesplayed, br_minutesplayed per playlist) from what its servers reported, but the 1.7.2 server reports only
    // its winners (battleroyale/recordwin, 0x72E130): UFortMcpUtils::RecordUserStatsUrl (/api/stats/bulk?ownertype=1)
    // has no caller left, so wins were the only stat. The rest comes from the player states: Place is set for every
    // player of a team when the team is out (TeamsLeft then, 0x44A870, from the death handler 0x437C30) or has won (1,
    // 0x400490 and 0x4008F0), with SecondsAlive and the player's Kills final by then. So they are read when a pawn of the
    // match dies (HandlePawnDied, the tick after its death handler), when a player leaves (HandlePlayerStateDestroyed)
    // and at each game phase (HandleGamePhaseChanged: the win ends the match), and each player is reported once, with
    // the DedicatedServer client's token. Core-Backend counts the win from recordwin, which 1.8 doesn't have
    // (Core-Backend #81): from a build without it, the report says to count a place 1 as the win.
    auto Results = std::make_shared<FMatchResults>();
    Results->GameMode = FWeakObjectPtr(this);
    Results->bCountWins = Finder::StringRefs(L"/api/game/v2/battleroyale/recordwin").empty();
    const FGuid MatchGuid = FGuid::NewGuid();
    Results->MatchId = std::format("{:08X}{:08X}{:08X}{:08X}", MatchGuid.A, MatchGuid.B, MatchGuid.C, MatchGuid.D);
    MatchResults = Results;
    Log::Info(L"Battle Royale: reporting the match's results to {} ({}{})", Configuration::Get().Backend,
        Results->bCountWins ? L"with its wins" : L"its wins through the build's recordwin",
        AFortPlayerStateAthena::CountsKills() ? L"" : AFortPlayerStateAthena::HasEliminator() ? L", kills counted by Core" : L", no kills: no DeathInfo");
#endif
}

namespace
{
    // How long after the backend's answer the server asks again for the admin panel's settings before its warmup, and
    // reports again during it. The admin panel and the backend's matchmaking queue change on their own, and Core-Backend
    // has no way to tell the server (no connection of its own to it), so the server has to ask: the next report is set
    // when the last one is answered, and a phase change reports at once.
    constexpr float WarmupAskInterval = 30.0f;
    constexpr float WarmupReportInterval = 1.0f;
    // How often a late player's reservation is looked at between their login and their joining the game.
    constexpr float LatePlayerInterval = 0.1f;

    struct FWarmupWatch
    {
        FWeakObjectPtr GameMode;
        bool bAsking = false;
        // A phase changed while a report was out: it is reported again as soon as that one is answered.
        bool bReportAgain = false;
        // The backend has no such route (one from before Core-Backend #122): it isn't asked again.
        bool bUnsupported = false;
        // The bus has left and the backend has heard so.
        bool bReportedEnd = false;
        // The next report, and the late players' next look.
        FTimerHandle ReportTimer;
        FTimerHandle LatePlayersTimer;

        // The backend's answer: the warmup's length (0: the game's own), whether it answered and has late joining on,
        // whether the matchmaking service sends parties here, the match's room, and the late parties it sent (their
        // account ids).
        int32_t WarmupSeconds = 0;
        bool bAnswered = false;
        bool bLateJoin = false;
        bool bOpen = false;
        int32_t MaxPlayers = 0;
        std::vector<std::vector<std::wstring>> Parties;
        // The match's players whose party plays without fill (the answer's noFill: the party leader's
        // player.option.fillTeam=false), who don't share a squad with others.
        std::set<std::wstring> NoFill;
        // Storm Destruction (CoreV2 #395): the answer's share of matches that get the mega storm (-1: none sent), and
        // whether this match has rolled for it (RollMegaStorm).
        int32_t MegaStormPercent = -1;
        bool bMegaStormRolled = false;

        // The game's own warmup lengths, from before Core first changed them (-1 until then), and the length the game
        // mode has now (0: the game's).
        float GameDuration = -1.0f;
        float GameEarly = -1.0f;
        int32_t AppliedSeconds = 0;
        // The game's warmup keeps its shorter countdown for a nearly full match: the match can't grow (no late joining)
        // or is of full size (MatchCapacity).
        bool bEarlyCountdown = false;
        bool bLoggedLength = false;
        // The running countdown was set to AppliedSeconds.
        bool bCountdownSet = false;
        bool bWarnedNoWarmupLength = false;

        // Late joining: the match was opened (MaxPlayerCount raised), the players who connected during the warmup
        // without a reservation, and those known to have their team (a reservation, and a player controller: PickTeam
        // has run).
        bool bOpened = false;
        bool bWarnedNoReservations = false;
        bool bWarnedNoTeam = false;
        std::set<std::wstring> LateIds;
        std::set<std::wstring> Settled;
        // 1.11: the squads Core gave late players (PlaceLatePlayerInSquad).
        std::set<uint8_t> LateSquads;
        // A large team game's late players who fill and the players' squad with room they were reserved a place by
        // (ReserveLatePlayers), which they join once the game has placed them on its team.
        std::map<std::wstring, uint8_t> FillSquads;

        // The room the game gave the game session before its match was assigned (KeepSessionRoom), and whether the
        // session has been given it back.
        int32_t SessionRoom = 0;
        bool bLoggedSessionRoom = false;
        // The session's room at the world's start, and its looks until the game creates its online session
        // (CheckSessionRoom).
        int32_t StartRoom = -1;
        int32_t SessionRoomChecks = 0;
        FTimerHandle SessionRoomTimer;
    };

    std::string NarrowId(const std::wstring& Id)
    {
        std::string Result;
        std::transform(Id.begin(), Id.end(), std::back_inserter(Result), [](wchar_t Character) { return static_cast<char>(Character); });
        return Result;
    }

    std::wstring WidenId(const std::string& Id)
    {
        return std::wstring(Id.begin(), Id.end());
    }

    // The answer's "parties": [["<accountId>", ...], ...].
    std::vector<std::vector<std::wstring>> JsonParties(const std::string& Json)
    {
        std::vector<std::vector<std::wstring>> Parties;
        const size_t Key = Json.find("\"parties\"");
        size_t Index = Key == std::string::npos ? std::string::npos : Json.find('[', Key);
        int32_t Depth = 0;
        for (; Index != std::string::npos && Index < Json.size(); ++Index)
        {
            const char Character = Json[Index];
            if (Character == '[')
            {
                if (++Depth == 2)
                {
                    Parties.emplace_back();
                }
            }
            else if (Character == ']')
            {
                if (--Depth == 0)
                {
                    break;
                }
            }
            else if (Character == '"' && Depth == 2)
            {
                const size_t End = Json.find('"', Index + 1);
                if (End == std::string::npos)
                {
                    break;
                }
                Parties.back().push_back(WidenId(Json.substr(Index + 1, End - Index - 1)));
                Index = End;
            }
        }
        return Parties;
    }

    // The reservation beacon's state (AFortGameSession's ReservationBeaconHost), or null.
    UPartyBeaconState* BeaconStateOf(AFortGameModeAthena* GameMode)
    {
        AFortGameSession* Session = AFortGameSession::HasReservationBeacon() ? Cast<AFortGameSession>(reinterpret_cast<UObject*>(static_cast<AGameSession*>(GameMode->GameSession))) : nullptr;
        APartyBeaconHost* Beacon = Session && APartyBeaconHost::IsReflected() ? static_cast<APartyBeaconHost*>(Session->ReservationBeaconHost) : nullptr;
        return Beacon ? static_cast<UPartyBeaconState*>(Beacon->State) : nullptr;
    }

    // The most players a match on this server can have: the playlist's teams, the reservation beacon's NumTeams x
    // NumPlayersPerTeam (GetMaxTeamInfoForPlaylist's, 1.10 0x7EA1F0: 100 x 1, 2 or 4, and 50v50's 2 x 50; the beacon has
    // them from the server's start and again when the match is assigned, "Reconfiguring to team count (2), team size
    // (50)"). Without a beacon, its game session's own MaxPlayers from its config (the class default: OnGivenMatchAssignment,
    // 1.9 0x42D2B0, overwrites the session's with the match's reservations), which is Save the World's on Battle Royale's
    // servers too ([/Script/Engine.GameSession] MaxPlayers=4 in 1.9.1's and 1.10's DefaultGame.ini). 0 when unknown.
    int32_t MatchCapacity(AFortGameModeAthena* GameMode)
    {
        if (UPartyBeaconState* State = BeaconStateOf(GameMode))
        {
            const int32_t Teams = State->NumTeams * State->NumPlayersPerTeam;
            if (Teams > 0)
            {
                return Teams;
            }
        }
        AGameSession* Session = static_cast<AGameSession*>(GameMode->GameSession);
        const UObject* Default = Session && AGameSession::HasMaxPlayers() ? Session->GetClass()->GetDefaultObject(false) : nullptr;
        return Default ? static_cast<const AGameSession*>(Default)->MaxPlayers : 0;
    }

    // The account ids of the players connected: their connections' (UNetConnection::PlayerID, from their login on, so
    // those still loading the map count), and the players' own player states in the game state's PlayerArray (a player
    // controller's, not a bot's: AGameModeBase::Logout takes it out). A 4.5 duo with fill (CoreV2) had its player on the
    // island and in PlayerArray with a valid UniqueId while no connection gave an account id: the bots didn't count them
    // (101 in the match) and the warmup report never named them, so no answer gave their fill choice and their team got
    // no bot. Each player is counted once, by either.
    std::vector<std::wstring> ConnectedAccounts(AFortGameModeAthena* GameMode)
    {
        std::vector<std::wstring> Ids;
        UWorld* World = GameMode->GetWorld();
        UNetDriver* Driver = World && UNetConnection::HasPlayerID() ? World->GetNetDriver() : nullptr;
        int32_t Connections = 0;
        std::wstring ConnectionIds;
        if (Driver)
        {
            for (UNetConnection* Connection : static_cast<TArray<UNetConnection*>&>(Driver->ClientConnections))
            {
                const bool bValid = Connection && Connection->PlayerID.IsValid();
                const std::wstring Id = bValid ? Connection->PlayerID.ToString().ToStdString() : std::wstring();
                ++Connections;
                // Shortened: the log is shared in issues.
                const std::wstring Shown = Id.size() > 8 ? Id.substr(0, 4) + L"..." + Id.substr(Id.size() - 4) : Id;
                ConnectionIds += std::format(L"{}{}", ConnectionIds.empty() ? L"" : L", ", !Connection ? L"none" : !bValid ? L"invalid" : Id.empty() ? L"empty" : Shown);
                if (IsAccountId(Id) && std::find(Ids.begin(), Ids.end(), Id) == Ids.end())
                {
                    Ids.push_back(Id);
                }
            }
        }
        AFortGameStateAthena* GameState = AthenaGameStateOf(GameMode);
        if (!GameState)
        {
            return Ids;
        }
        const size_t FromConnections = Ids.size();
        for (APlayerState* PlayerState : static_cast<const TArray<APlayerState*>&>(GameState->PlayerArray))
        {
            AFortPlayerStateAthena* AthenaState = Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(PlayerState));
            if (!AthenaState || !Cast<AFortPlayerControllerAthena>(reinterpret_cast<UObject*>(AthenaState->GetOwner())))
            {
                continue;
            }
            const std::wstring Id = AthenaState->UniqueId.IsValid() ? AthenaState->UniqueId.ToString().ToStdString() : std::wstring();
            if (IsAccountId(Id) && std::find(Ids.begin(), Ids.end(), Id) == Ids.end())
            {
                Ids.push_back(Id);
            }
        }
        // Once: why the connections missed a player (the next match's log says which part didn't give their id).
        static bool bLoggedMissed = false;
        if (Ids.size() > FromConnections && !bLoggedMissed)
        {
            bLoggedMissed = true;
            Log::Warning(L"Battle Royale: {} players are in the game state but not among the connections' ids (net driver {}, PlayerID {}, {} client connections: {}); "
                L"they count from their player states", Ids.size() - FromConnections, Driver ? L"found" : World && World->GetNetDriver() ? L"found" : L"none",
                UNetConnection::HasPlayerID() ? L"found" : L"not found", Connections, ConnectionIds.empty() ? L"-" : ConnectionIds);
        }
        return Ids;
    }

    // A JSON object's array of strings Key: ["<accountId>", ...] (none when it isn't there).
    std::vector<std::wstring> JsonStringArray(const std::string& Json, const char* Key)
    {
        std::vector<std::wstring> Values;
        const std::string Quoted = std::format("\"{}\"", Key);
        const size_t Found = Json.find(Quoted);
        const size_t Open = Found == std::string::npos ? std::string::npos : Json.find_first_not_of(" \t\r\n:", Found + Quoted.size());
        if (Open == std::string::npos || Json[Open] != '[')
        {
            return Values;
        }
        for (size_t Index = Open + 1; Index < Json.size() && Json[Index] != ']'; ++Index)
        {
            if (Json[Index] != '"')
            {
                continue;
            }
            const size_t End = Json.find('"', Index + 1);
            if (End == std::string::npos)
            {
                break;
            }
            Values.push_back(WidenId(Json.substr(Index + 1, End - Index - 1)));
            Index = End;
        }
        return Values;
    }

    // The answer's "bots": {count, level, health, shield, quests, fights, teams, noFill}, or null when the admin panel's
    // "Force player AIs" and "Fill players' teams with bots" are both off (Core-Backend's bots block). False when it isn't
    // there or is null.
    bool JsonBots(const std::string& Json, FCoreAthenaBotSettings& OutSettings)
    {
        const size_t Key = Json.find("\"bots\"");
        const size_t Open = Key == std::string::npos ? std::string::npos : Json.find_first_not_of(" \t\r\n:", Key + 6);
        if (Open == std::string::npos || Json[Open] != '{')
        {
            return false;
        }
        const size_t Close = Json.find('}', Open);
        const std::string Bots = Json.substr(Open, Close == std::string::npos ? std::string::npos : Close - Open + 1);
        OutSettings.Count = std::clamp(std::atoi(BackendRequest::JsonField(Bots, "count").c_str()), 0, 100);
        OutSettings.Level = std::clamp(std::atoi(BackendRequest::JsonField(Bots, "level").c_str()), 1, 10);
        const std::string Health = BackendRequest::JsonField(Bots, "health");
        const std::string Shield = BackendRequest::JsonField(Bots, "shield");
        OutSettings.StartingHealth = Health.empty() ? 100.0f : std::clamp(static_cast<float>(std::atof(Health.c_str())), 1.0f, 100.0f);
        OutSettings.StartingShield = Shield.empty() ? 0.0f : std::clamp(static_cast<float>(std::atof(Shield.c_str())), 0.0f, 100.0f);
        OutSettings.bEliminationsCountForQuests = BackendRequest::JsonField(Bots, "quests") != "false";
        // A backend from before the setting sends none: the old 30%.
        const std::string Fights = BackendRequest::JsonField(Bots, "fights");
        OutSettings.BotFightChance = Fights.empty() ? 30 : std::clamp(std::atoi(Fights.c_str()), 0, 100);
        // "Fill players' teams with bots", and the players whose party plays without fill (a backend from before the
        // setting sends neither: off).
        OutSettings.bFillPlayerTeams = BackendRequest::JsonField(Bots, "teams") == "true";
        OutSettings.NoFillAccounts = JsonStringArray(Bots, "noFill");
        return OutSettings.Count > 0 || OutSettings.bFillPlayerTeams;
    }

    void WarmupAnswered(const std::shared_ptr<FWarmupWatch>& Watch);

    void ReportWarmup(const std::shared_ptr<FWarmupWatch>& Watch, const std::wstring& Phase, const std::vector<std::wstring>& AccountIds, float SecondsLeft)
    {
        std::string Ids;
        for (const std::wstring& Id : AccountIds)
        {
            Ids += std::format("{}\"{}\"", Ids.empty() ? "" : ",", NarrowId(Id));
        }
        Watch->bAsking = true;
        BackendRequest::PostJson(L"Battle Royale warmup report", L"/fortnite/api/game/v2/matchmakingservice/warmup",
            std::format("{{\"phase\":\"{}\",\"accountIds\":[{}],\"secondsLeft\":{:.1f}}}", NarrowId(Phase), Ids, std::max(SecondsLeft, 0.0f)),
            FFortMcpConfig::DedicatedServerClientId, FFortMcpConfig::DedicatedServerClientSecret, [Watch, AccountIds](uint32_t Status, const std::string& Body)
        {
            // {warmupSeconds: a number or null, lateJoin, sessionId, open, maxPlayers, parties}. Anything else (no
            // answer) keeps what the server has.
            const std::string Seconds = Status == 200 ? BackendRequest::JsonField(Body, "warmupSeconds") : std::string();
            const bool bAnswered = Seconds == "null" || (!Seconds.empty() && std::isdigit(static_cast<unsigned char>(Seconds[0])));
            const int32_t WarmupSeconds = bAnswered && Seconds != "null" ? std::clamp(std::atoi(Seconds.c_str()), 0, 3600) : 0;
            const bool bLateJoin = bAnswered && BackendRequest::JsonField(Body, "lateJoin") == "true";
            const bool bOpen = bAnswered && BackendRequest::JsonField(Body, "open") == "true";
            const int32_t MaxPlayers = bAnswered ? std::clamp(std::atoi(BackendRequest::JsonField(Body, "maxPlayers").c_str()), 0, 1000) : 0;
            auto Parties = bAnswered ? JsonParties(Body) : std::vector<std::vector<std::wstring>>();
            FCoreAthenaBotSettings BotSettings;
            const bool bBots = bAnswered && JsonBots(Body, BotSettings);
            // The match's no-fill players (Core-Backend's top-level noFill, #242; a backend from before it sends them only
            // in the bots block).
            std::vector<std::wstring> NoFill = bAnswered ? JsonStringArray(Body, "noFill") : std::vector<std::wstring>();
            // The share of matches with Storm Destruction (CoreV2 #395; a backend from before it sends none).
            const std::string StormPercent = bAnswered ? BackendRequest::JsonField(Body, "stormDestructionPercent") : std::string();
            const int32_t MegaStormPercent = !StormPercent.empty() && std::isdigit(static_cast<unsigned char>(StormPercent[0]))
                ? std::clamp(std::atoi(StormPercent.c_str()), 0, 100) : -1;
            GameThread::Enqueue([Watch, AccountIds, Status, bAnswered, WarmupSeconds, bLateJoin, bOpen, MaxPlayers, Parties = std::move(Parties), bBots, BotSettings, NoFill = std::move(NoFill),
                MegaStormPercent]() mutable
            {
                Watch->bAsking = false;
                if (bAnswered)
                {
                    if (FCoreAthenaBotManager* BotManager = FCoreAthenaBotManager::Get())
                    {
                        BotManager->SetSettings(bBots, BotSettings);
                        // The players this report named: their fill choice is in this answer (CoreV2 #330).
                        BotManager->SetFillAnswered(AccountIds);
                    }
                }
                if (Status == 404 && !Watch->bUnsupported)
                {
                    Watch->bUnsupported = true;
                    Log::Warning(L"Battle Royale: the backend has no warmup settings (Core-Backend #122), so the match keeps the game's warmup and takes no late players");
                }
                if (bAnswered)
                {
                    Watch->WarmupSeconds = WarmupSeconds;
                    Watch->bAnswered = true;
                    Watch->bLateJoin = bLateJoin;
                    Watch->bOpen = bOpen;
                    Watch->MaxPlayers = MaxPlayers;
                    Watch->Parties = std::move(Parties);
                    Watch->NoFill = std::set<std::wstring>(NoFill.begin(), NoFill.end());
                    Watch->MegaStormPercent = MegaStormPercent;
                }
                WarmupAnswered(Watch);
            });
        });
    }

    // The game mode's warmup lengths: the backend's or the game's own, and a countdown already running moved to the
    // backend's. Either way the countdown still shortens to the game's early countdown once the match's players are all
    // in, as it did before the panel's setting (the backend's length only replaces the full one). The game shortens its warmup to
    // WarmupEarlyCountdownDuration (15 s) once enough of MaxPlayerCount is in (UpdateWarmupPhase, 1.9 0x43D9A0:
    // NumPlayers >= MaxPlayerCount times a percent at +0xABC, which isn't reflected), and OnGivenMatchAssignment
    // (0x42D2B0) sets MaxPlayerCount to the players the match was assigned. So a match taking late players had its bus
    // leave 15 s into the game's 90 s warmup, before the backend's answer opened it (Core-Backend #122). With the
    // game's length the shorter countdown is the full one while the match may still grow: until the backend says late
    // joining is off (or has no warmup settings), when the assigned players are all the match gets and the countdown
    // shortens once they are in (#153), or until MaxPlayerCount is the most a match here can have (MatchCapacity), as
    // when late joining opened it to them; the game's percent then counts a match of full size, as on Epic's servers.
    void ApplyWarmupLength(AFortGameModeAthena* GameMode, AFortGameStateAthena* GameState, FWarmupWatch& Watch, float Now)
    {
        if (!AFortGameModeAthena::HasWarmupLength())
        {
            if (Watch.WarmupSeconds > 0 && !Watch.bWarnedNoWarmupLength)
            {
                Watch.bWarnedNoWarmupLength = true;
                Log::Warning(L"Battle Royale: this build reflects no WarmupCountdownDuration, so the warmup keeps the game's length");
            }
            return;
        }

        if (Watch.GameDuration < 0.0f)
        {
            Watch.GameDuration = GameMode->WarmupCountdownDuration;
            Watch.GameEarly = GameMode->WarmupEarlyCountdownDuration;
        }
        const bool bGame = Watch.WarmupSeconds <= 0;
        const float Duration = bGame ? Watch.GameDuration : static_cast<float>(Watch.WarmupSeconds);
        const int32_t Capacity = MatchCapacity(GameMode);
        const bool bClosed = Watch.bUnsupported || (Watch.bAnswered && !Watch.bLateJoin);
        // With player bots on, the warmup is in full whatever the players (CoreV2 #256): the match fills with bots, which
        // come in one at a time over it, and a countdown shortened to 15 s brought them all in a burst that hung the server.
        const FCoreAthenaBotManager* BotManager = FCoreAthenaBotManager::Get();
        const bool bBots = BotManager && BotManager->ShouldKeepFullWarmup();
        const bool bEarly = !bBots && (bClosed || (AFortGameModeAthena::HasMaxPlayerCount() && Capacity > 0 && GameMode->MaxPlayerCount >= Capacity));
        GameMode->WarmupCountdownDuration = Duration;
        GameMode->WarmupEarlyCountdownDuration = bEarly ? std::min(Watch.GameEarly, Duration) : Duration;

        if (!Watch.bLoggedLength || Watch.WarmupSeconds != Watch.AppliedSeconds || bEarly != Watch.bEarlyCountdown)
        {
            if (Watch.WarmupSeconds != Watch.AppliedSeconds)
            {
                Watch.bCountdownSet = bGame;
            }
            Watch.bLoggedLength = true;
            Watch.AppliedSeconds = Watch.WarmupSeconds;
            Watch.bEarlyCountdown = bEarly;
            if (!bGame)
            {
                Log::Info(L"Battle Royale: the warmup lasts {} s (the backend's), {}", Watch.WarmupSeconds,
                    bEarly ? std::format(L"{:.0f} s once the match's players are all in", std::min(Watch.GameEarly, Duration))
                        : std::wstring(bBots ? L"in full while the player bots come in" : L"in full while late players can still join"));
            }
            else if (bBots)
            {
                Log::Info(L"Battle Royale: the warmup is the game's {:.0f} s in full while the player bots come in", Watch.GameDuration);
            }
            else if (bEarly)
            {
                Log::Info(L"Battle Royale: the warmup is the game's: {:.0f} s, {:.0f} s once enough of the match's {} players are in{}", Watch.GameDuration, Watch.GameEarly,
                    AFortGameModeAthena::HasMaxPlayerCount() ? GameMode->MaxPlayerCount : 0, bClosed ? L" (no late joining)" : L"");
            }
            else
            {
                Log::Info(L"Battle Royale: the warmup is the game's {:.0f} s in full (its {:.0f} s countdown once enough players are in is for a match of {} players)", Watch.GameDuration, Watch.GameEarly, Capacity);
            }
        }

        // UpdateWarmupPhase (1.9 0x43D9A0) sets the countdown once, when the warmup starts: one that started before the
        // backend answered takes the backend's length from its start.
        if (!Watch.bCountdownSet && Watch.AppliedSeconds > 0 && GameState->GamePhase == EAthenaGamePhase::GetWarmup() && AFortGameStateAthena::HasWarmupCountdown() && GameState->WarmupCountdownEndTime >= 0.0f)
        {
            Watch.bCountdownSet = true;
            const float End = std::max(GameState->WarmupCountdownStartTime + static_cast<float>(Watch.AppliedSeconds), Now + 1.0f);
            Log::Info(L"Battle Royale: the warmup's countdown ends in {:.0f} s (was {:.0f} s)", End - Now, GameState->WarmupCountdownEndTime - Now);
            GameState->WarmupCountdownEndTime = End;
        }
    }

    // Late joining: the match lets in as many players as the backend gives it room for. The game mode lets in only the
    // assigned players (OnGivenMatchAssignment, 1.9 0x42D2B0, sets MaxPlayerCount and the game session's MaxPlayers
    // from the beacon's reservations), PreLogin turning anyone else away ("Match is full", 0x4319F0) until the bus
    // leaves ("Match has started": StartAircraftPhase sets the flag, 0x4388CB). Raised during the warmup only: the
    // match has started then (HandleMatchHasStarted, 0x427D90, clears the failed assignment timer, which kicks everyone
    // when it fires, 0x42D100). The playlist's teams cap it.
    void OpenMatch(AFortGameModeAthena* GameMode, FWarmupWatch& Watch)
    {
        UPartyBeaconState* State = BeaconStateOf(GameMode);
        AGameSession* Session = static_cast<AGameSession*>(GameMode->GameSession);
        if (!State || !Session || !AFortGameModeAthena::HasMaxPlayerCount() || !AGameSession::HasMaxPlayers() || !UNetConnection::HasPlayerID())
        {
            if (!Watch.bWarnedNoReservations)
            {
                Watch.bWarnedNoReservations = true;
                Log::Warning(L"Battle Royale: no reservation beacon, MaxPlayerCount or connection player id on this build, so the match takes no late players");
            }
            return;
        }
        const int32_t Room = std::min(Watch.MaxPlayers, State->NumTeams * State->NumPlayersPerTeam);
        if (Room <= 0)
        {
            return;
        }
        if (Room > GameMode->MaxPlayerCount || Room > Session->MaxPlayers)
        {
            Log::Info(L"Battle Royale: the match takes late players during its warmup, up to {} (MaxPlayerCount was {}, the session's MaxPlayers {})", Room, GameMode->MaxPlayerCount, Session->MaxPlayers);
            GameMode->MaxPlayerCount = std::max(GameMode->MaxPlayerCount, Room);
            Session->MaxPlayers = std::max(Session->MaxPlayers, Room);
        }
        Watch.bOpened = true;
    }

    // The game session's room (AGameSession::MaxPlayers) once the match has started (#234). AGameSession::ApproveLogin
    // (3.0 0x2030960) turns a joining player away with "Server full." when AtCapacity (0x2030BD0) finds GetNumPlayers()
    // at MaxPlayers, at PreLogin and again at Login, after the player has loaded the map. The game gives the session the
    // playlist's room when it creates it (GetMaxTeamInfoForPlaylist, 3.0 0xD67E60, MaxPlayers 100 for Battle Royale's
    // playlists), which is the maxPlayers its MMS ticket asks for, and then the MMS match assignment lowers it to the
    // players it reserved ("MMS - configuring beacon for match assignment", 3.0 0xCEBE20 and 2.5 0xCD1080:
    // ReservationBeaconHost->GetNumConsumedReservations()), so only a match the backend filled at once had room for all
    // its players. Who may join is the game mode's to say (AFortGameModeAthena::PreLogin, 3.0 0x9B95B0: "Match is full"
    // past MaxPlayerCount, "Match has started" once the bus has left), so from the warmup on the session keeps the room
    // the game gave it, up to the playlist's teams: a player the game mode lets in is never "Server full." below the
    // playlist's limit, however the match got them.
    void KeepSessionRoom(AFortGameModeAthena* GameMode, FWarmupWatch& Watch, uint8_t Phase)
    {
        AGameSession* Session = static_cast<AGameSession*>(GameMode->GameSession);
        if (!Session || !AGameSession::HasMaxPlayers())
        {
            return;
        }
        if (Phase < EAthenaGamePhase::GetWarmup())
        {
            Watch.SessionRoom = std::max(Watch.SessionRoom, static_cast<int32_t>(Session->MaxPlayers));
            return;
        }
        const int32_t Capacity = MatchCapacity(GameMode);
        const int32_t Room = Capacity > 0 ? std::min(Watch.SessionRoom, Capacity) : Watch.SessionRoom;
        if (Room > Session->MaxPlayers)
        {
            if (!Watch.bLoggedSessionRoom)
            {
                Watch.bLoggedSessionRoom = true;
                Log::Info(L"Battle Royale: the game session has room for {} players again (the match assignment left it {})", Room, Session->MaxPlayers);
            }
            Session->MaxPlayers = Room;
        }
    }

    // 1.11: a late player's squad (AFortPlayerStateAthena::SquadId). AFortGameModeAthena::PlacePlayerOnTeam (2.1
    // 0x444780) gives a joining player their team (AFortGameMode's) and then, on a dedicated session, their squad from
    // the session's squads by account id (0x7300A0), which only the match assignment fills: each player of its teams[i]
    // is in squad i + 1 (MMS's beacon setup, 0x738440). A player Core reserved a place for isn't in them, so their
    // squad stayed 0, none: the squad bar and its voice list left them out (OnRep_SquadId, 0x45D790, skips squad 0),
    // while the server's SquadMembers fill (0x433FD0) and SpectateOnDeath (0x44C6F0), which compare squads only, took
    // every other squadless player for a squadmate, whatever their team.
    // So once the game has placed them they join their party's squad when a member of it is on their team, as the
    // assignment puts a party in one squad, else a squad of their own. The assignment's squads count up from 1 and its
    // players may still be loading, so Core's count down from 255, clear of them. Returns whether the player is placed.
    bool PlaceLatePlayerInSquad(AFortGameModeAthena* GameMode, FWarmupWatch& Watch, const std::wstring& Id, APlayerController* Controller)
    {
        if (!AFortPlayerStateAthena::HasSquadId())
        {
            return true;
        }
        AFortPlayerStateAthena* State = Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(static_cast<APlayerState*>(Controller->PlayerState)));
        AFortGameStateAthena* GameState = AthenaGameStateOf(GameMode);
        // Not placed yet: the player state starts on Spectator (2.1 0x48BFD0).
        if (!State || !GameState || State->TeamIndex == EFortTeam::GetSpectator())
        {
            return false;
        }
        if (State->SquadId != 0)
        {
            return true;
        }

        const auto Party = std::find_if(Watch.Parties.begin(), Watch.Parties.end(), [&](const std::vector<std::wstring>& Members) { return std::find(Members.begin(), Members.end(), Id) != Members.end(); });
        std::set<uint8_t> Used = Watch.LateSquads;
        uint8_t Squad = 0;
        // A large team game's player who fills: the players' squad ReserveLatePlayers found them, when it's on their team.
        const auto Fill = Watch.FillSquads.find(Id);
        bool bFillSquadThere = false;
        const TArray<APlayerState*>& PlayerArray = GameState->PlayerArray;
        for (APlayerState* PlayerState : PlayerArray)
        {
            AFortPlayerStateAthena* Other = Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(PlayerState));
            const uint8_t OtherSquad = Other && Other != State ? static_cast<uint8_t>(Other->SquadId) : 0;
            if (OtherSquad == 0)
            {
                continue;
            }
            Used.insert(OtherSquad);
            const std::wstring OtherId = Other->UniqueId.ToString().ToStdString();
            if (Squad == 0 && Party != Watch.Parties.end() && Other->TeamIndex == State->TeamIndex && std::find(Party->begin(), Party->end(), OtherId) != Party->end())
            {
                Squad = OtherSquad;
            }
            bFillSquadThere = bFillSquadThere || (Fill != Watch.FillSquads.end() && OtherSquad == Fill->second && Other->TeamIndex == State->TeamIndex);
        }
        if (Squad == 0 && bFillSquadThere)
        {
            Squad = Fill->second;
        }
        if (Fill != Watch.FillSquads.end())
        {
            Watch.FillSquads.erase(Fill);
        }
        for (uint32_t Candidate = 255; Squad == 0 && Candidate > 0; --Candidate)
        {
            Squad = Used.contains(static_cast<uint8_t>(Candidate)) ? 0 : static_cast<uint8_t>(Candidate);
        }
        if (Squad == 0)
        {
            Log::Warning(L"Battle Royale: no squad is free for {}, so they have none", Id);
            return true;
        }
        Watch.LateSquads.insert(Squad);
        State->SquadId = Squad;
        State->ForceNetUpdate();
        Log::Info(L"Battle Royale: {} is in squad {} on {}", Id, Squad, static_cast<EFortTeam>(State->TeamIndex).ToString());
        return true;
    }

    // A large team game's (Teams of 20: five teams of several squads of 4) place for a late player whose party fills
    // (#242: they played on another team than the players already in, and alone in their squad): as the matchmaking
    // service fills squads before it shares them out between the teams (Core-Backend formTeams), the players' squad with
    // the most players that still has room for their party and no player who doesn't fill, on a team with room; on a
    // tie, the team with the fewest players. Its squad is kept for PlaceLatePlayerInSquad; the beacon's team (0 for HumanPvP_Team1) is
    // returned, or INDEX_NONE (another kind of playlist, 50v50's two even sides among them, a player who doesn't fill, or
    // no such squad): the team the beacon state picks.
    int32_t FindFillSquad(AFortGameModeAthena* GameMode, FWarmupWatch& Watch, UPartyBeaconState* BeaconState, const std::wstring& Id, int32_t PartySize)
    {
        AFortGameStateAthena* GameState = AthenaGameStateOf(GameMode);
        const int32_t SquadSize = FCoreAthenaBotManager::MaxSquadSize;
        if (!GameState || !AFortPlayerStateAthena::HasSquadId() || BeaconState->NumPlayersPerTeam <= SquadSize || BeaconState->NumTeams <= 2
            || Watch.NoFill.contains(Id))
        {
            return INDEX_NONE;
        }
        struct FSquad
        {
            int32_t Players = 0;
            bool bNoFill = false;
        };
        const FCoreAthenaBotManager* BotManager = FCoreAthenaBotManager::Get();
        const uint8_t FirstTeam = EFortTeam::GetHumanPvP_Team1();
        std::map<std::pair<uint8_t, uint8_t>, FSquad> Squads;
        for (APlayerState* PlayerState : static_cast<const TArray<APlayerState*>&>(GameState->PlayerArray))
        {
            AFortPlayerStateAthena* Other = Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(PlayerState));
            if (!Other || Other->TeamIndex == EFortTeam::GetSpectator() || static_cast<uint8_t>(Other->SquadId) == 0
                || (BotManager && BotManager->FindBot(reinterpret_cast<const UObject*>(Other))))
            {
                continue;
            }
            FSquad& Squad = Squads[{ static_cast<uint8_t>(Other->TeamIndex), static_cast<uint8_t>(Other->SquadId) }];
            ++Squad.Players;
            Squad.bNoFill = Squad.bNoFill || Watch.NoFill.contains(Other->UniqueId.ToString().ToStdString());
        }
        // The late players already sent to a squad and still loading count in it.
        for (const auto& [Pending, PendingSquad] : Watch.FillSquads)
        {
            for (auto& [Key, Squad] : Squads)
            {
                Squad.Players += Pending != Id && Key.second == PendingSquad ? 1 : 0;
            }
        }
        std::pair<uint8_t, uint8_t> Best{ 0, 0 };
        int32_t BestPlayers = 0;
        int32_t BestTeamPlayers = 0;
        for (const auto& [Key, Squad] : Squads)
        {
            const int32_t Team = static_cast<int32_t>(Key.first) - FirstTeam;
            const int32_t TeamPlayers = Team >= 0 ? BeaconState->GetNumPlayersOnTeam(Team) : BeaconState->NumPlayersPerTeam;
            if (Squad.bNoFill || Squad.Players + PartySize > SquadSize || TeamPlayers + PartySize > BeaconState->NumPlayersPerTeam)
            {
                continue;
            }
            if (Squad.Players > BestPlayers || (Squad.Players == BestPlayers && TeamPlayers < BestTeamPlayers))
            {
                Best = Key;
                BestPlayers = Squad.Players;
                BestTeamPlayers = TeamPlayers;
            }
        }
        if (BestPlayers == 0)
        {
            return INDEX_NONE;
        }
        Watch.FillSquads[Id] = Best.second;
        Log::Info(L"Battle Royale: {} fills squad {} on {} ({} players in it)", Id, Best.second, EFortTeam{ Best.first }.ToString(), BestPlayers);
        return static_cast<int32_t>(Best.first) - FirstTeam;
    }

    // Gives each player who joined during the warmup a team. Battle Royale's clients never talk to the beacon, so only
    // the assigned players have a reservation, and PickTeam (1.9 0x430A50) gives the beacon's team to anyone on a
    // dedicated session's match: for a player without one that is Spectator (0x6FF3D0 answers 103), which has no
    // place in the match. So each such player gets a reservation of their own between their login and their joining
    // the game (map load, then Login and PickTeam): on their party's reservation when the backend sent their party, a
    // member already holds one and its team has room, else on a reservation of their own on the team the beacon state's
    // GetTeamAssignment (Smallest) picks: the team with the fewest players that has room. A team holds one party on solo,
    // duo and squad's 100 teams (an empty one is the smallest), and several on 50v50's two teams of 50. The beacon's
    // reservations are its state's reflected array (UPartyBeaconState::Reservations), the ids copied from the
    // connection with the game's own struct copy, and a player is looked at again until they have a player controller,
    // in case the beacon dropped one that took too long to load (APartyBeaconHost's session timeout), and from 1.11
    // until they have a squad (PlaceLatePlayerInSquad).
    void ReserveLatePlayers(AFortGameModeAthena* GameMode, FWarmupWatch& Watch, bool bWarmup)
    {
        UPartyBeaconState* State = BeaconStateOf(GameMode);
        UWorld* World = GameMode->GetWorld();
        UNetDriver* Driver = World ? World->GetNetDriver() : nullptr;
        static UScriptStruct* const IdStruct = FindObject<UScriptStruct>(nullptr, L"/Script/Engine.UniqueNetIdRepl");
        if (!State || !Driver || !IdStruct)
        {
            return;
        }

        std::vector<std::pair<std::wstring, UNetConnection*>> Unsettled;
        for (UNetConnection* Connection : static_cast<TArray<UNetConnection*>&>(Driver->ClientConnections))
        {
            const std::wstring Id = Connection && Connection->PlayerID.IsValid() ? Connection->PlayerID.ToString().ToStdString() : std::wstring();
            // After the bus has left a new player is turned away ("Match has started"): only those who came before.
            if (IsAccountId(Id) && !Watch.Settled.contains(Id) && (bWarmup || Watch.LateIds.contains(Id)))
            {
                Unsettled.emplace_back(Id, Connection);
            }
        }
        if (Unsettled.empty())
        {
            return;
        }

        TStructArray<FPartyReservation>& Reservations = State->GetMember_Reservations();
        std::map<std::wstring, int32_t> ReservationOf;
        for (int32_t Index = 0; Index < Reservations.Num(); ++Index)
        {
            FPartyReservation& Reservation = Reservations[Index];
            TStructArray<FPlayerReservation>& Members = Reservation.GetMember_PartyMembers();
            for (int32_t Member = 0; Member < Members.Num(); ++Member)
            {
                ReservationOf[Members[Member].UniqueId.ToString().ToStdString()] = Index;
            }
        }

        for (const auto& [Id, Connection] : Unsettled)
        {
            APlayerController* const Controller = static_cast<APlayerController*>(Connection->PlayerController);
            const bool bJoined = Controller != nullptr;
            if (ReservationOf.contains(Id))
            {
                if (bJoined && (!Watch.LateIds.contains(Id) || PlaceLatePlayerInSquad(GameMode, Watch, Id, Controller)))
                {
                    Watch.Settled.insert(Id);
                }
                continue;
            }
            if (bJoined)
            {
                // In the game before Core saw them: PickTeam has run.
                Watch.Settled.insert(Id);
                Log::Warning(L"Battle Royale: {} joined without a reservation, so PickTeam made them a Spectator", Id);
                continue;
            }
            Watch.LateIds.insert(Id);

            // Shooting Test #1 (playlist 15, Core-Backend #189) keeps squads' 100 teams of 4 in the server's team table, and
            // from 2.4.2 matchmaking splits parties into a team each: a party-mate's reservation of one is a solo team,
            // which a late player doesn't join.
            const bool bSoloReservations = GameMode->CurrentPlaylistId == 15;
            int32_t Index = -1;
            const auto Party = std::find_if(Watch.Parties.begin(), Watch.Parties.end(), [&](const std::vector<std::wstring>& Members) { return std::find(Members.begin(), Members.end(), Id) != Members.end(); });
            for (const std::wstring& Member : Party != Watch.Parties.end() ? *Party : std::vector<std::wstring>())
            {
                const auto Found = ReservationOf.find(Member);
                if (Found != ReservationOf.end() && State->GetNumPlayersOnTeam(Reservations[Found->second].TeamNum) < State->NumPlayersPerTeam
                    && !(bSoloReservations && Reservations[Found->second].GetMember_PartyMembers().Num() < 2))
                {
                    Index = Found->second;
                    break;
                }
            }
            if (Index < 0)
            {
                // A large team game's player who fills (Teams of 20, #242): a players' squad with room, on its team.
                const int32_t FillTeam = FindFillSquad(GameMode, Watch, State, Id, Party != Watch.Parties.end() ? static_cast<int32_t>(Party->size()) : 1);
                // The new reservation is on no team until it has one, so the teams' counts leave it out.
                Index = Reservations.Num();
                FPartyReservation& Reservation = Reservations.AddDefaulted();
                Reservation.TeamNum = INDEX_NONE;
                IdStruct->CopyScriptStruct(&Reservation.GetMember_PartyLeader(), &Connection->GetMember_PlayerID());
                FPlayerReservation& Leader = Reservation.GetMember_PartyMembers().AddDefaulted();
                IdStruct->CopyScriptStruct(&Leader.GetMember_UniqueId(), &Connection->GetMember_PlayerID());
                const int32_t Team = FillTeam != INDEX_NONE ? FillTeam : State->GetTeamAssignment(Reservation);
                if (Team == INDEX_NONE)
                {
                    Reservations.RemoveAt(Index);
                    if (!Watch.bWarnedNoTeam)
                    {
                        Watch.bWarnedNoTeam = true;
                        Log::Warning(L"Battle Royale: none of the {} teams has room, so {} has no team", State->NumTeams, Id);
                    }
                    continue;
                }
                Reservation.TeamNum = Team;
            }
            else
            {
                FPlayerReservation& Member = Reservations[Index].GetMember_PartyMembers().AddDefaulted();
                IdStruct->CopyScriptStruct(&Member.GetMember_UniqueId(), &Connection->GetMember_PlayerID());
            }
            FPartyReservation& Reservation = Reservations[Index];
            ReservationOf[Id] = Index;
            Log::Info(L"Battle Royale: {} joins in the warmup on HumanPvP_Team{} (beacon team {}, {} of its party there)", Id, Reservation.TeamNum + 1, Reservation.TeamNum,
                Reservation.GetMember_PartyMembers().Num());
        }
    }

    // This world's warmup reports (StartWarmupReports).
    std::shared_ptr<FWarmupWatch> Warmup;

    std::shared_ptr<FWarmupWatch> WarmupOf(const AFortGameModeAthena* GameMode)
    {
        return Warmup && Warmup->GameMode.Get() == GameMode ? Warmup : nullptr;
    }

    // KeepSessionRoom keeps the room the game gave the game session. The game gives it when it creates its online
    // session, a few seconds after the world's start (3.3: MaxPlayers 4, the config's, at FinishWorldInitialization and
    // 100 some 8 s later), and the match assignment lowers it to the players it reserved, which can come right after
    // (a player already in the queue). Both happen inside the game session (AFortGameSession's session creation and its
    // MMS assignment), with nothing broadcast that Core can bind, so the room is looked at every SessionRoomInterval
    // from the world's start until it changes (the session is created), the warmup starts or SessionRoomLooks are over.
    constexpr float SessionRoomInterval = 0.5f;
    constexpr int32_t SessionRoomLooks = 120;

    void CheckSessionRoom(AFortGameModeAthena* GameMode)
    {
        std::shared_ptr<FWarmupWatch> Watch = WarmupOf(GameMode);
        AGameSession* Session = Watch && AGameSession::HasMaxPlayers() ? static_cast<AGameSession*>(GameMode->GameSession) : nullptr;
        AFortGameStateAthena* GameState = Session ? AthenaGameStateOf(GameMode) : nullptr;
        if (!GameState)
        {
            return;
        }
        Watch->SessionRoomTimer.Invalidate();
        const int32_t Room = Session->MaxPlayers;
        Watch->StartRoom = Watch->StartRoom < 0 ? Room : Watch->StartRoom;
        if (GameState->GamePhase >= EAthenaGamePhase::GetWarmup())
        {
            return;
        }
        Watch->SessionRoom = std::max(Watch->SessionRoom, Room);
        if (Room == Watch->StartRoom && ++Watch->SessionRoomChecks < SessionRoomLooks)
        {
            SetGameModeTimer(GameMode, Watch->SessionRoomTimer, SessionRoomInterval, &CheckSessionRoom);
        }
        else if (Room != Watch->StartRoom)
        {
            Log::Info(L"Battle Royale: the game session has room for {} players (it had {} at the world's start)", Room, Watch->StartRoom);
        }
    }

    // Whether a player who came during the warmup without a reservation still has to join the game (or, from 1.11, to
    // get their squad).
    bool HasUnsettledLatePlayers(const FWarmupWatch& Watch)
    {
        return std::any_of(Watch.LateIds.begin(), Watch.LateIds.end(), [&](const std::wstring& Id) { return !Watch.Settled.contains(Id); });
    }

    void UpdateLatePlayers(AFortGameModeAthena* GameMode);

    // What the server keeps up from the backend's answer and the game phase: the game session's room, the warmup's
    // length, the match opened to late players and their reservations. Each answer, phase change and late player's look
    // runs it.
    void UpdateWarmup(AFortGameModeAthena* GameMode, FWarmupWatch& Watch)
    {
        AFortGameStateAthena* GameState = AthenaGameStateOf(GameMode);
        UWorld* World = GameMode->GetWorld();
        if (!GameState || !World)
        {
            return;
        }
        const uint8_t Phase = GameState->GamePhase;
        const bool bWarmup = Phase == EAthenaGamePhase::GetWarmup();

        KeepSessionRoom(GameMode, Watch, Phase);
        if (Phase <= EAthenaGamePhase::GetWarmup())
        {
            ApplyWarmupLength(GameMode, GameState, Watch, World->GetTimeSeconds());
        }
        if (bWarmup && Watch.bOpen)
        {
            OpenMatch(GameMode, Watch);
        }
        if (!Watch.bOpened)
        {
            return;
        }
        ReserveLatePlayers(GameMode, Watch, bWarmup);
        // A late player is reserved a place as soon as a report sees their connection, and has to keep it until PickTeam
        // has given them its team: their Login, after they have loaded the map. No event Core can bind comes in between
        // on every build (AGameModeBase's PreLogin and PostLogin events, FGameModeEvents, are static native delegates), so
        // while one is between the two the reservations are looked at every LatePlayerInterval (ReserveLatePlayers), up
        // to the storm: everyone who came during the warmup is in the game by then.
        if (HasUnsettledLatePlayers(Watch) && Phase <= EAthenaGamePhase::GetAircraft())
        {
            if (!Watch.LatePlayersTimer.IsValid())
            {
                SetGameModeTimer(GameMode, Watch.LatePlayersTimer, LatePlayerInterval, &UpdateLatePlayers);
            }
        }
    }

    void UpdateLatePlayers(AFortGameModeAthena* GameMode)
    {
        if (std::shared_ptr<FWarmupWatch> Watch = WarmupOf(GameMode))
        {
            Watch->LatePlayersTimer.Invalidate();
            UpdateWarmup(GameMode, *Watch);
        }
    }

    // Every 30 seconds before the warmup (the settings), every second during it (the match's players and time left),
    // once after.
    void ReportWarmupNow(AFortGameModeAthena* GameMode)
    {
        std::shared_ptr<FWarmupWatch> Watch = WarmupOf(GameMode);
        AFortGameStateAthena* GameState = Watch ? AthenaGameStateOf(GameMode) : nullptr;
        UWorld* World = GameMode->GetWorld();
        if (!GameState || !World || Watch->bAsking || Watch->bUnsupported || Watch->bReportedEnd)
        {
            return;
        }
        ClearGameModeTimer(GameMode, Watch->ReportTimer);
        const uint8_t Phase = GameState->GamePhase;
        const bool bWarmup = Phase == EAthenaGamePhase::GetWarmup();
        const bool bAfter = Phase > EAthenaGamePhase::GetWarmup();
        Watch->bReportedEnd = bAfter;
        float SecondsLeft = 0.0f;
        if (bWarmup && AFortGameStateAthena::HasWarmupCountdown())
        {
            SecondsLeft = GameState->WarmupCountdownEndTime >= 0.0f ? GameState->WarmupCountdownEndTime - World->GetTimeSeconds()
                : AFortGameModeAthena::HasWarmupLength() ? GameMode->WarmupCountdownDuration : 0.0f;
        }
        if (bAfter)
        {
            Watch->bOpen = false;
        }
        ReportWarmup(Watch, GameState->GamePhase.ToString(), ConnectedAccounts(GameMode), SecondsLeft);
    }

    // The report's timer: its handle is spent once it runs.
    void ReportWarmupOnTimer(AFortGameModeAthena* GameMode)
    {
        if (std::shared_ptr<FWarmupWatch> Watch = WarmupOf(GameMode))
        {
            Watch->ReportTimer.Invalidate();
            ReportWarmupNow(GameMode);
        }
    }

    // Storm Destruction (CoreV2 #395, Core-Backend's events/stormdestruction.js): 5.40's mega storm, whose circles on the
    // storm's edge flash and destroy the structures there in the last four circles. The game runs all of it:
    // AFortGameModeAthena::HandleMatchHasStarted (5.40 0xBEEAD0) spawns its MegaStormManagerClass, StartNewSafeZonePhase
    // (0xC030B0) starts the circles for a circle whose MegaStormGridCellThickness is 1 or more (AthenaGameData's
    // Default.SafeZone.06 to 09) while FortAthenaGameModeCVars::bMegaStormEnabled, the console variable
    // Athena.bMegaStormEnabled, is set, and AMegaStormManager::Tick (0xC364C0) destroys what they gather. That variable is
    // Epic's switch: 5.40's and 5.41's DefaultEngine.ini set it, 6.00's clears it, and Core-Backend's hotfix sets it by
    // Epic's dates or the admin panel. Epic rolled no chance per match (its "about 10% of matches" was the share that got
    // that far), so the admin panel's share of matches is Core's own: each match rolls once against the warmup answer's
    // stormDestructionPercent while the switch is on, and one that doesn't get it has the switch off until the next match
    // rolls. It rolls when the storm starts (the safe zones phase), as the game only reads the switch at each new circle
    // and the server's hotfix can land after the first answers (a 5.40 bots-only match: its [ConsoleVariables] 5 s after
    // HandleMatchHasStarted). A build without the mega storm (no MegaStormManager class) and a switch the hotfix or the
    // paks left off are left as they are.
    void RollMegaStorm(AFortGameModeAthena* GameMode, FWarmupWatch& Watch)
    {
        AFortGameStateAthena* GameState = AthenaGameStateOf(GameMode);
        if (Watch.bMegaStormRolled || Watch.MegaStormPercent < 0 || !GameState || static_cast<uint8_t>(GameState->GamePhase) < EAthenaGamePhase::GetSafeZones())
        {
            return;
        }
        Watch.bMegaStormRolled = true;
        static UClass* MegaStormManagerClass = FindObject<UClass>(nullptr, L"/Script/FortniteGame.MegaStormManager");
        IConsoleVariable* MegaStormEnabled = MegaStormManagerClass ? IConsoleManager::Get().FindConsoleVariable(L"Athena.bMegaStormEnabled") : nullptr;
        if (!MegaStormEnabled)
        {
            return;
        }
        // The switch as the hotfix (or the paks) set it: the value it has now, unless it is still the one Core last set.
        static int32_t SwitchValue = -1;
        static int32_t CoreValue = -1;
        const int32_t Value = MegaStormEnabled->GetInt() != 0 ? 1 : 0;
        if (CoreValue < 0 || Value != CoreValue)
        {
            SwitchValue = Value;
        }
        if (!SwitchValue)
        {
            Log::Info(L"Battle Royale: no Storm Destruction (Athena.bMegaStormEnabled is off)");
            return;
        }
        const int32_t Percent = Watch.MegaStormPercent;
        const bool bThisMatch = Percent >= 100 || (Percent > 0 && FMath::FRand() * 100.0f < static_cast<float>(Percent));
        MegaStormEnabled->SetWithCurrentPriority(bThisMatch ? 1 : 0);
        CoreValue = MegaStormEnabled->GetInt() != 0 ? 1 : 0;
        Log::Info(L"Battle Royale: Storm Destruction {} this match ({}% of matches; Athena.bMegaStormEnabled {})", bThisMatch ? L"comes" : L"doesn't come",
            Percent, CoreValue);
    }

    // The backend answered (or the request failed): the server takes the answer, then reports again, at once when a
    // phase changed meanwhile, else after the interval of the phase it is in.
    void WarmupAnswered(const std::shared_ptr<FWarmupWatch>& Watch)
    {
        AFortGameModeAthena* GameMode = static_cast<AFortGameModeAthena*>(Watch->GameMode.Get());
        AFortGameStateAthena* GameState = GameMode && WarmupOf(GameMode) == Watch ? AthenaGameStateOf(GameMode) : nullptr;
        if (!GameState)
        {
            return;
        }
        UpdateWarmup(GameMode, *Watch);
        if (Watch->bUnsupported || Watch->bReportedEnd)
        {
            return;
        }
        if (Watch->bReportAgain)
        {
            Watch->bReportAgain = false;
            ReportWarmupNow(GameMode);
            return;
        }
        const bool bWarmup = GameState->GamePhase == EAthenaGamePhase::GetWarmup();
        SetGameModeTimer(GameMode, Watch->ReportTimer, bWarmup ? WarmupReportInterval : WarmupAskInterval, &ReportWarmupOnTimer);
    }

    // A new game phase: the server keeps its room and length up, and the backend hears of it at once (after the report
    // that is out, if one is). The storm starting rolls the match's Storm Destruction.
    void WarmupPhaseChanged(AFortGameModeAthena* GameMode)
    {
        std::shared_ptr<FWarmupWatch> Watch = WarmupOf(GameMode);
        if (!Watch)
        {
            return;
        }
        RollMegaStorm(GameMode, *Watch);
        UpdateWarmup(GameMode, *Watch);
        if (Watch->bAsking)
        {
            Watch->bReportAgain = true;
            return;
        }
        ReportWarmupNow(GameMode);
    }
}

void AFortGameModeAthena::StartWarmupReports()
{
#if WITH_SERVER_CODE
    // Core-Backend #122: the admin panel's warmup length and late joining. The warmup lasts the game mode's
    // WarmupCountdownDuration, a config value no match option sets (1.9's OnGivenMatchAssignment, 0x42D2B0, reads only
    // match.option.warmupRequiredPlayerCount; 1.9.1's, 0x42CF40, adds customKey, isMatchStatsDisabled and
    // isProgressionDisabled). And the matchmaking service can't add players to a match: the server's MMS client knows
    // only Registered, AssignMatch and UnassignMatch (0x3BF2730), takes an AssignMatch only while Registered, and
    // leaves the service once its match is assigned (0x709370). So the server asks the backend (POST
    // matchmakingservice/warmup) from its world's start and sets the warmup's length; with late joining on it opens
    // its match during the warmup, reports its players and time left every second so the backend sends it the parties
    // that queue, and gives each of them a team.
    Warmup = std::make_shared<FWarmupWatch>();
    Warmup->GameMode = FWeakObjectPtr(this);
    UpdateWarmup(this, *Warmup);
    CheckSessionRoom(this);
    ReportWarmupNow(this);
#endif
}

namespace
{
    // How long after each answer a match asks for the calendar's revision again. Epic's calendar changed only on its
    // own schedule, which a running match fetched again at the calendar's cacheExpire; Core-Backend's admin panel
    // changes it whenever an admin does, and the backend has no way to tell the server, so the server asks.
    constexpr float EventCalendarAskInterval = 5.0f;

    struct FEventCalendarWatch
    {
        FWeakObjectPtr GameMode;
        bool bAsking = false;
        // The backend has no such route (one from before it): it isn't asked again.
        bool bUnsupported = false;
        // The revision of the calendar the server last fetched (empty until the first answer).
        std::string Revision;
        FTimerHandle AskTimer;
    };

    // This world's (AskEventCalendarRevision).
    std::shared_ptr<FEventCalendarWatch> EventCalendar;
}

void AFortGameModeAthena::AskEventCalendarRevision()
{
#if WITH_SERVER_CODE
    if (!AFortGameState::FindStaticFunction_Client_RefreshEventCalendar())
    {
        return;
    }
    if (!EventCalendar || EventCalendar->GameMode.Get() != this)
    {
        EventCalendar = std::make_shared<FEventCalendarWatch>();
        EventCalendar->GameMode = FWeakObjectPtr(this);
    }
    const std::shared_ptr<FEventCalendarWatch> Watch = EventCalendar;
    if (Watch->bAsking || Watch->bUnsupported)
    {
        return;
    }
    Watch->bAsking = true;
    Watch->AskTimer.Invalidate();
    BackendRequest::GetJson(L"event calendar revision", L"/fortnite/api/game/v2/calendar/revision",
        FFortMcpConfig::DedicatedServerClientId, FFortMcpConfig::DedicatedServerClientSecret, [Watch](uint32_t Status, const std::string& Body)
    {
        std::string Revision = Status == 200 ? BackendRequest::JsonField(Body, "revision") : std::string();
        GameThread::Enqueue([Watch, Status, Revision = std::move(Revision)]()
        {
            Watch->bAsking = false;
            AFortGameModeAthena* Athena = static_cast<AFortGameModeAthena*>(Watch->GameMode.Get());
            if (!Athena || EventCalendar != Watch)
            {
                return;
            }
            if (Status == 404)
            {
                Watch->bUnsupported = true;
                Log::Info(L"Event calendar: the backend has no calendar revision, so a running match fetches the calendar only at its cacheExpire");
                return;
            }
            // The first answer is the calendar the server fetched as it started.
            const bool bChanged = !Revision.empty() && !Watch->Revision.empty() && Revision != Watch->Revision;
            Watch->Revision = Revision.empty() ? Watch->Revision : Revision;
            AFortGameState* GameState = static_cast<AFortGameState*>(AthenaGameStateOf(Athena));
            if (bChanged && GameState && GameState->Client_RefreshEventCalendar())
            {
                Log::Info(L"Event calendar: the admin panel changed it, so the server and its players fetch it again");
            }
            SetGameModeTimer(Athena, Watch->AskTimer, EventCalendarAskInterval, [](AFortGameModeAthena* GameMode) { GameMode->AskEventCalendarRevision(); });
        });
    });
#endif
}

int32_t AFortGameModeAthena::GetMatchCapacity()
{
    return MatchCapacity(this);
}

int32_t AFortGameModeAthena::GetNumConnectedPlayers()
{
    return static_cast<int32_t>(ConnectedAccounts(this).size());
}

int32_t AFortGameModeAthena::GetPlayersPerTeam()
{
    UPartyBeaconState* State = BeaconStateOf(this);
    return State && State->NumPlayersPerTeam > 0 ? static_cast<int32_t>(State->NumPlayersPerTeam) : 0;
}

int32_t AFortGameModeAthena::GetNumTeams()
{
    UPartyBeaconState* State = BeaconStateOf(this);
    return State && State->NumTeams > 0 ? static_cast<int32_t>(State->NumTeams) : 0;
}

std::vector<std::pair<uint8_t, std::wstring>> AFortGameModeAthena::GetTeamReservations()
{
    std::vector<std::pair<uint8_t, std::wstring>> Players;
    UPartyBeaconState* State = UPartyBeaconState::IsReflected() ? BeaconStateOf(this) : nullptr;
    if (!State)
    {
        return Players;
    }
    const int32_t FirstTeam = EFortTeam::GetHumanPvP_Team1();
    const TStructArray<FPartyReservation>& Reservations = State->GetMember_Reservations();
    for (int32_t Index = 0; Index < Reservations.Num(); ++Index)
    {
        const FPartyReservation& Reservation = Reservations[Index];
        if (Reservation.TeamNum < 0 || FirstTeam + Reservation.TeamNum > 0xFF)
        {
            continue;
        }
        const TStructArray<FPlayerReservation>& Members = Reservation.GetMember_PartyMembers();
        for (int32_t Member = 0; Member < Members.Num(); ++Member)
        {
            if (Members[Member].UniqueId.IsValid())
            {
                Players.emplace_back(static_cast<uint8_t>(FirstTeam + Reservation.TeamNum), Members[Member].UniqueId.ToString().ToStdString());
            }
        }
    }
    return Players;
}

namespace
{
    // The game mode whose playlist's tables ApplyPlaylistDataTables last copied over the default ones, and that playlist.
    FWeakObjectPtr TablesGameMode;
    int32_t TablesPlaylistId = -1;
    // The loot tables that playlist copied over the default ones.
    std::pair<UDataTable*, UDataTable*> TablesLoot;
    // The game mode whose vending machines wait for the match's playlist (SpawnVendingMachines).
    FWeakObjectPtr VendingGameMode;

    void SpawnMapVendingMachines(AFortGameModeAthena* GameMode)
    {
        AFortAthenaMapInfo* MapInfo = AFortAthenaMapInfo::StaticClass() && AFortAthenaMapInfo::HasVendingMachines() ? FindMapInfo(GameMode->GetWorld()) : nullptr;
        if (MapInfo)
        {
            MapInfo->SpawnVendingMachines();
        }
    }

    // The vending machines that waited for the match's playlist roll now, from the world's tables as they are.
    void SpawnWaitingVendingMachines(AFortGameModeAthena* GameMode, const std::wstring& Why)
    {
        if (VendingGameMode.Get() != GameMode)
        {
            return;
        }
        VendingGameMode = FWeakObjectPtr();
        Log::Info(L"Battle Royale: the vending machines roll {}", Why);
        SpawnMapVendingMachines(GameMode);
    }

    // The default Athena loot tables' rows, copied before any playlist's tables replaced them. From 3.5 ApplyPlaylistDataTables
    // (3.5.1 0x9DC890) empties AthenaLootTierData_Client and AthenaLootPackages_Client and copies the playlist's rows in
    // (UDataTable::CreateTableFromOtherTable, 0x20FA4F0, which starts with EmptyTable, 0x20FE5E0). A playlist's tables
    // are older copies of the defaults: none has 4.0's Hop Rocks (Loot_ForagedItem_GravityRocks) or meteor drops, and
    // Blitz has no vending or llama loot. Their rows come back from the defaults for the tier groups (and loot packages)
    // the playlist's tables have none of, so nothing rolls into nothing (#291, #292); the playlist's own groups stay its.
    struct FDefaultLootRows
    {
        const wchar_t* Path;
        UScriptStruct* Struct = nullptr;
        std::vector<std::pair<FName, uint8_t*>> Rows;
    };
    FDefaultLootRows DefaultTierRows{ L"/Game/Items/Datatables/AthenaLootTierData_Client.AthenaLootTierData_Client" };
    FDefaultLootRows DefaultPackageRows{ L"/Game/Items/Datatables/AthenaLootPackages_Client.AthenaLootPackages_Client" };

    // The playlists' tables are in the game state's CurrentPlaylistData from 3.5; before it (PlaylistDataTables) they
    // keep the game's own rows.
    bool MergesDefaultLootRows()
    {
        return !AFortGameModeAthena::HasPlaylistDataTables() && AFortGameStateAthena::HasCurrentPlaylist();
    }

    // Copies the default tables' rows, once per process, before the first playlist's tables replace them.
    void TakeDefaultLootRows()
    {
        static bool bTaken = false;
        if (bTaken || !MergesDefaultLootRows())
        {
            return;
        }
        bTaken = true;
        for (FDefaultLootRows* Defaults : { &DefaultTierRows, &DefaultPackageRows })
        {
            UDataTable* Table = LoadObject<UDataTable>(nullptr, Defaults->Path);
            UScriptStruct* Struct = Table ? static_cast<UScriptStruct*>(Table->RowStruct) : nullptr;
            if (!Struct)
            {
                continue;
            }
            Defaults->Struct = Struct;
            for (auto& Pair : Table->GetRowMap())
            {
                uint8_t* Copy = static_cast<uint8_t*>(FMemory::Malloc(Struct->GetStructureSize()));
                Struct->InitializeStruct(Copy);
                Struct->CopyScriptStruct(Copy, Pair.Value);
                Defaults->Rows.emplace_back(Pair.Key, Copy);
            }
        }
    }

    // Adds back the default rows whose key (a tier row's TierGroup, a package row's LootPackageID) the table has no row of.
    template <class RowType, class KeyOf>
    int32_t MergeDefaultRows(const FDefaultLootRows& Defaults, KeyOf Key)
    {
        UDataTable* Table = Defaults.Struct ? LoadObject<UDataTable>(nullptr, Defaults.Path) : nullptr;
        if (!Table || static_cast<UScriptStruct*>(Table->RowStruct) != Defaults.Struct)
        {
            return 0;
        }
        std::unordered_set<FName> Present;
        for (auto& Pair : Table->GetRowMap())
        {
            Present.insert(Key(*reinterpret_cast<RowType*>(Pair.Value)));
        }
        int32_t Added = 0;
        for (const auto& [RowName, Row] : Defaults.Rows)
        {
            if (Present.contains(Key(*reinterpret_cast<RowType*>(Row))) || Table->FindRowUnchecked(RowName))
            {
                continue;
            }
            if (uint8_t* NewRow = Table->AddRow(RowName))
            {
                Defaults.Struct->CopyScriptStruct(NewRow, Row);
                ++Added;
            }
        }
        return Added;
    }

    void MergeDefaultLootRows(int32_t PlaylistId)
    {
        if (!MergesDefaultLootRows())
        {
            return;
        }
        const int32_t Tiers = MergeDefaultRows<FFortLootTierData>(DefaultTierRows, [](FFortLootTierData& Row) { return static_cast<FName>(Row.TierGroup); });
        const int32_t Packages = MergeDefaultRows<FFortLootPackageData>(DefaultPackageRows, [](FFortLootPackageData& Row) { return static_cast<FName>(Row.LootPackageID); });
        if (Tiers > 0 || Packages > 0)
        {
            Log::Info(L"Battle Royale: playlist {}'s loot tables get {} loot tier and {} loot package rows of the defaults, for the tier groups and packages they have none of", PlaylistId, Tiers, Packages);
        }
    }

    // The loot tables PlaylistId's PlaylistDataTables entry copies over the default ones (none: the defaults).
    std::pair<UDataTable*, UDataTable*> PlaylistLootTables(AFortGameModeAthena* GameMode, int32_t PlaylistId)
    {
        if (!AFortGameModeAthena::HasPlaylistDataTables())
        {
            // 3.5: ApplyPlaylistDataTables (3.5.1 0x9DC890) loads the game state's CurrentPlaylistData's tables.
            AFortGameStateAthena* GameState = AFortGameStateAthena::HasCurrentPlaylist() ? AthenaGameStateOf(GameMode) : nullptr;
            UFortPlaylistAthena* Playlist = GameState ? GameState->GetCurrentPlaylist() : nullptr;
            if (Playlist && Playlist->PlaylistId == PlaylistId)
            {
                return { Playlist->LootTierData.LoadSynchronous(), Playlist->LootPackages.LoadSynchronous() };
            }
            return { nullptr, nullptr };
        }
        TStructArray<FAthenaPlaylistDataTableSet>& Sets = GameMode->PlaylistDataTables;
        for (int32_t Index = 0; Index < Sets.Num(); ++Index)
        {
            FAthenaPlaylistDataTableSet& Set = Sets[Index];
            if (FAthenaPlaylistDataTableSet::HasPlaylistId())
            {
                if (Set.PlaylistId == PlaylistId)
                {
                    return { Set.LootTierData, Set.LootPackages };
                }
                continue;
            }
            // 3.0: ApplyPlaylistDataTables (3.0 0x99AB50) reads the playlist class's default object.
            UClass* PlaylistClass = FAthenaPlaylistDataTableSet::HasFortAthenaPlaylistClass() ? Set.FortAthenaPlaylistClass : nullptr;
            AFortAthenaPlaylistBase* Playlist = PlaylistClass && PlaylistClass->IsChildOf(AFortAthenaPlaylistBase::StaticClass())
                ? static_cast<AFortAthenaPlaylistBase*>(PlaylistClass->GetDefaultObject()) : nullptr;
            if (Playlist && Playlist->PlaylistId == PlaylistId)
            {
                return { Playlist->LootTierData, Playlist->LootPackages };
            }
        }
        return { nullptr, nullptr };
    }

    // The playlist whose tables ApplyPlaylistDataTables copies now: the game mode's CurrentPlaylistId, and from 3.5 the
    // game state's CurrentPlaylistData (-1 while it has none, when it copies nothing). The game state has it only from
    // the match's assignment on: OnGivenMatchAssignment (3.5.1 0x9EF530) calls SetCurrentPlaylistId (0x9FA020) with the
    // game mode's CurrentPlaylistId, which looks it up in the game mode's PlaylistManager, and its assets load before
    // OnPlaylistDataLoadCompleted (0x9EFE50) has the game mode apply its tables. So on 3.5 CurrentPlaylistId changed
    // before the tables could, the watcher took the default tables for 50v50 v2's (#260), and its world kept the
    // default loot.
    int32_t AppliedPlaylistId(AFortGameModeAthena* GameMode)
    {
        if (AFortGameModeAthena::HasPlaylistDataTables() || !AFortGameStateAthena::HasCurrentPlaylist())
        {
            return GameMode->CurrentPlaylistId;
        }
        AFortGameStateAthena* GameState = AthenaGameStateOf(GameMode);
        UFortPlaylistAthena* Playlist = GameState ? GameState->GetCurrentPlaylist() : nullptr;
        return Playlist ? static_cast<int32_t>(Playlist->PlaylistId) : -1;
    }

    // The aircraft paths are the match's playlist's. The world's playlist built them: OnPlaylistDataLoaded (4.1 0xA2F500)
    // builds the map info's FlightInfos (0xA2B8F0) once, while bFlightPathInitialized is false, and sets it, and the world
    // starts on the map URL's playlist, the match's coming with its assignment. The paths are one per team with
    // OpposingAirCraftForEachTeam (AirCraftBehavior 1, which SetCurrentPlaylistId, 0xA6DD20, gives playlists 50 and 11,
    // with TeamCount the playlist's MaxTeamCount), else one, and StartAircraftPhase (0xA3B530) spawns TeamCount aircraft
    // with no check that a path is there: each takes FlightInfos[its index] (0xA3A830). So 50v50 v2 had one path for two
    // buses (#295): the second took memory past the array, never reached its drop zone, and OnAircraftEnteredDropZone
    // (0xA2E350) unlocks the aircraft (bAircraftIsLocked, which the jump checks, 0xA2C750) only once every aircraft has,
    // so nobody could jump until the first bus dropped everyone; nor did the match reach SafeZones, which
    // OnAircraftExitedDropZone (0xA2E400) starts once every aircraft has left its drop zone, so the HUD's timer (0xA740D0)
    // stayed at the aircraft phase's 0:00. With the flag cleared the game builds them again, for the match's playlist and
    // its game data, at the next OnPlaylistDataLoaded or at StartAircraftPhase (both empty FlightInfos first), as it does
    // when the world starts on the match's playlist. From 3.5 (OpposingAirCraftForEachTeam): 3.5.1 to 4.1 have the same
    // checks. 4.2's StartAircraftPhase (0xA28610) no longer builds them, and the next OnPlaylistDataLoaded may never come,
    // so there the game's own build runs now (AFortGameModeAthena::InitializeFlightPath): the game state already has the
    // match's playlist (AppliedPlaylistId reads it), its AirCraftBehavior and TeamCount with it.
    void RebuildFlightPathsFor(AFortGameModeAthena* GameMode, int32_t PlaylistId, int32_t WorldPlaylistId)
    {
        if (WorldPlaylistId < 0 || !AFortGameModeAthena::HasFlightPathInitialized() || !AFortGameStateAthena::HasAircrafts() || !GameMode->bFlightPathInitialized)
        {
            return;
        }
        GameMode->bFlightPathInitialized = false;
        if (GameMode->InitializeFlightPath())
        {
            Log::Info(L"Battle Royale: the aircraft paths were built for playlist {}, so they are built again for the match's playlist {}", WorldPlaylistId, PlaylistId);
            return;
        }
        Log::Info(L"Battle Royale: the aircraft paths were built for playlist {}, so the game builds them again for the match's playlist {}", WorldPlaylistId, PlaylistId);
    }

    // The match's playlist, once its assignment gives the world another one than its tables are: the tables are
    // applied (the game's ApplyPlaylistDataTables; the game mode's own call to it is the same rows), and the loot is
    // rolled again with them before the players come. Only while the match is in its warmup or before.
    void ApplyMatchPlaylist(AFortGameModeAthena* Athena)
    {
        AFortGameStateAthena* GameState = AthenaGameStateOf(Athena);
        if (TablesGameMode.Get() != Athena || !GameState || GameState->GamePhase >= EAthenaGamePhase::GetAircraft())
        {
            return;
        }
        const int32_t PlaylistId = AppliedPlaylistId(Athena);
        if (PlaylistId == TablesPlaylistId || PlaylistId < 0)
        {
            return;
        }
        const int32_t Previous = TablesPlaylistId;
        TablesPlaylistId = PlaylistId;
        RebuildFlightPathsFor(Athena, PlaylistId, Previous);
        Natives::AFortGameModeAthena_ApplyPlaylistDataTables(Athena);
        MergeDefaultLootRows(PlaylistId);
        // 5.0+: the searches and floor loot pick this many times (Building/BuildingContainer.cpp, #357).
        if (UFortPlaylistAthena* Playlist = UFortPlaylistAthena::HasLootDropRounds() && AFortGameStateAthena::HasCurrentPlaylist() ? GameState->GetCurrentPlaylist() : nullptr;
            Playlist && Playlist->LootDropRounds > 1)
        {
            Log::Info(L"Battle Royale: playlist {}'s loot drops {} rounds a search and floor loot spot", PlaylistId, static_cast<int32_t>(Playlist->LootDropRounds));
        }
        const std::pair<UDataTable*, UDataTable*> Tables = PlaylistLootTables(Athena, PlaylistId);
        const bool bOtherTables = Tables != TablesLoot;
        TablesLoot = Tables;
        if (bOtherTables)
        {
            Log::Info(L"Battle Royale: the match's playlist {} has other loot tables than playlist {}, so the world's loot is rolled again", PlaylistId, Previous);
            ABuildingContainer::RollAthenaLootAgain(Athena->GetWorld());
            ABGAConsumableSpawner::SpawnWorldConsumables(Athena->GetWorld());
        }
        SpawnWaitingVendingMachines(Athena, std::format(L"from the match's playlist {}'s tables", PlaylistId));
    }

    // How often the match's playlist is looked for where the game state broadcasts nothing when it has it (before 4.4).
    constexpr float MatchPlaylistCheckInterval = 0.5f;

    // Before 4.4: Epic applies the match's playlist's tables in the game mode's OnGivenMatchAssignment (to 3.3) or its
    // OnPlaylistDataLoaded (3.5 to 4.2), virtual functions the game session and the game state call, which those builds
    // keep (Core doesn't hook intact code), and the game state broadcasts nothing then: OnCurrentPlaylistLoaded comes with
    // 4.4. The match's playlist is CurrentPlaylistId once the matchmaking service's assignment is being handled
    // (HandlingMatchAssignment), but OnGivenMatchAssignment runs only when the server has answered it (Finished), some
    // 20 s later on 2.4.2; from 3.5 the tables wait for the game state's playlist (AppliedPlaylistId). So until the warmup
    // ends the playlist is looked for every MatchPlaylistCheckInterval, and applied as soon as it is there, so the loot is
    // rolled again before the players come. The bus leaving with no playlist rolls the vending machines from the
    // default tables (HandleGamePhaseChanged).
    void CheckMatchPlaylist(AFortGameModeAthena* Athena)
    {
        FMatchEvents& Events = MatchEventsOf(Athena);
        Events.PlaylistTimer.Invalidate();
        AFortGameStateAthena* GameState = AthenaGameStateOf(Athena);
        if (TablesGameMode.Get() != Athena || !GameState || GameState->GamePhase >= EAthenaGamePhase::GetAircraft())
        {
            return;
        }
        ApplyMatchPlaylist(Athena);
        SetGameModeTimer(Athena, Events.PlaylistTimer, MatchPlaylistCheckInterval, &CheckMatchPlaylist);
    }

    // From 4.4, the tick after the game state's OnCurrentPlaylistLoaded.
    void AfterCurrentPlaylistLoaded(AFortGameModeAthena* Athena)
    {
        MatchEventsOf(Athena).PlaylistTimer.Invalidate();
        ApplyMatchPlaylist(Athena);
    }
}

namespace
{
    struct FTeamSwitchWatch
    {
        FWeakObjectPtr GameMode;
        // Each player state's team at the last check.
        std::map<const UObject*, uint8_t> Teams;
        bool bLoggedPlaylist = false;
    };

    // This world's.
    FTeamSwitchWatch TeamSwitches;

    // Playground's team switching (4.2 onwards; UFortPlaylistAthena's bAllowTeamSwitching). The switch is the game's:
    // 5.10's AFortPlayerControllerAthena::SetTeam_Implementation (0xC0DD10) calls the game mode's ChangeTeam (slot 0xA80,
    // 0xDC1310), whose AttemptChangeTeamInfoForController (0xDC1410) has SetTeamInfo (0xDE8240) take the controller out
    // of the old AFortTeamInfo (RemoveFromTeam, 0xED0C00: TeamMembers and the AFortTeamPrivateInfo's entry) and into
    // the new one (AddToTeam, 0xEB9120: TeamMembers, the player state's SetPlayerTeam with PlayerTeam, PlayerTeamPrivate
    // and TeamIndex, and the new AFortTeamPrivateInfo's entry, whose health and shield the game mode's
    // ReplicateHealthAndShield keeps up), then kills the pawn so the player comes back on the new team. Nothing in it
    // changes the player state's SquadId, which the match assignment gave (each team's players in squad index + 1), and
    // nothing in it broadcasts (the player state's OnRep_TeamIndex, 5.40 0xC60330, is the client's alone): so the switch
    // is seen at the pawn's death (HandlePawnDied) and as the player comes back (OnPlayerPossessedPawn).
    bool HasTeamSwitching()
    {
        return AFortPlayerStateAthena::HasSquadId() && UFortPlaylistAthena::HasAllowTeamSwitching() && AFortGameStateAthena::HasCurrentPlaylist();
    }

    // A player state the game moved to another team since the last check joins the new team's squad: a player's squad
    // on it (else a bot's, else one no player state has), and the team's other player states without it (bots, whose
    // own teams have none) join it too, as a Playground team is one squad (MaxSquads 1). Replicated at once, so the
    // clients' OnRep_SquadId moves the player between squad bars.
    void CheckTeamSwitches(AFortGameModeAthena* GameMode)
    {
        if (!HasTeamSwitching())
        {
            return;
        }
        FTeamSwitchWatch& Watch = TeamSwitches;
        if (Watch.GameMode.Get() != GameMode)
        {
            Watch = FTeamSwitchWatch();
            Watch.GameMode = FWeakObjectPtr(GameMode);
        }
        AFortGameStateAthena* GameState = AthenaGameStateOf(GameMode);
        UFortPlaylistAthena* Playlist = GameState ? GameState->GetCurrentPlaylist() : nullptr;
        if (!Playlist || !Playlist->bAllowTeamSwitching)
        {
            Watch.Teams.clear();
            return;
        }
        if (!Watch.bLoggedPlaylist)
        {
            Watch.bLoggedPlaylist = true;
            Log::Info(L"Battle Royale: the playlist allows team switching, so a player who changes teams joins the new team's squad");
        }
        const uint8_t Spectator = EFortTeam::GetSpectator();
        std::vector<AFortPlayerStateAthena*> States;
        for (APlayerState* PlayerState : static_cast<const TArray<APlayerState*>&>(GameState->PlayerArray))
        {
            if (AFortPlayerStateAthena* AthenaState = Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(PlayerState)))
            {
                States.push_back(AthenaState);
            }
        }
        std::map<const UObject*, uint8_t> Teams;
        for (AFortPlayerStateAthena* State : States)
        {
            const uint8_t Team = static_cast<uint8_t>(State->TeamIndex);
            Teams[State] = Team;
            const auto Known = Watch.Teams.find(State);
            if (Known == Watch.Teams.end() || Known->second == Team || Known->second == Spectator || Team == Spectator)
            {
                continue;
            }
            const auto IsPlayer = [](AFortPlayerStateAthena* Other) { return Cast<APlayerController>(reinterpret_cast<UObject*>(Other->GetOwner())) != nullptr; };
            uint8_t Squad = 0;
            uint8_t BotSquad = 0;
            std::set<uint8_t> Used;
            for (AFortPlayerStateAthena* Other : States)
            {
                const uint8_t OtherSquad = static_cast<uint8_t>(Other->SquadId);
                if (Other == State || OtherSquad == 0)
                {
                    continue;
                }
                Used.insert(OtherSquad);
                if (static_cast<uint8_t>(Other->TeamIndex) != Team)
                {
                    continue;
                }
                uint8_t& Found = IsPlayer(Other) ? Squad : BotSquad;
                if (Found == 0)
                {
                    Found = OtherSquad;
                }
            }
            Squad = Squad ? Squad : BotSquad;
            for (uint32_t Candidate = 255; Squad == 0 && Candidate > 0; --Candidate)
            {
                Squad = Used.contains(static_cast<uint8_t>(Candidate)) ? 0 : static_cast<uint8_t>(Candidate);
            }
            if (Squad == 0)
            {
                continue;
            }
            const uint8_t OldSquad = static_cast<uint8_t>(State->SquadId);
            int32_t Joined = 0;
            for (AFortPlayerStateAthena* Other : States)
            {
                if ((Other == State || static_cast<uint8_t>(Other->TeamIndex) == Team) && static_cast<uint8_t>(Other->SquadId) != Squad
                    && (Other == State || static_cast<uint8_t>(Other->SquadId) == 0 || !IsPlayer(Other)))
                {
                    Other->SquadId = Squad;
                    Other->ForceNetUpdate();
                    Joined += Other == State ? 0 : 1;
                }
            }
            Log::Info(L"Battle Royale: {} switched from {} to {}, so they leave squad {} for squad {}{}", State->GetPlayerName().ToStdString(), EFortTeam{ Known->second }.ToString(),
                EFortTeam{ Team }.ToString(), OldSquad, Squad, Joined > 0 ? std::format(L" ({} of the team's bots join it too)", Joined) : std::wstring());
        }
        Watch.Teams = std::move(Teams);
    }
}

bool AFortGameModeAthena::IsRespawningAllowed(AFortPlayerStateAthena* PlayerState)
{
    // From 6.01 the build's own, intact and virtual (HookVTables finds its slot), which is what the pawn's death handler
    // goes by when it has the player respawn (6.02 AFortPlayerPawnAthena 0xC486D0: "call [rax+9A8h]", then the respawn
    // time and RespawnPlayerAfterDeath's timer). Core's reading of the playlist below said no for a playlist whose
    // mutator brings the players back (Playlist_Disco has no RespawnType), so each death in Disco Domination was also
    // an elimination: a place, the alive players' count and the spectating, for a player the game then respawned. And
    // it said yes where a mutator says no on a playlist that has a RespawnType (AFortAthenaMutator_Barrier answers by
    // team, 6.02 0xC780C0): the game respawned nobody, and the player was never out.
    const int32_t Index = Natives::AFortGameModeAthena_IsRespawningAllowedIndex;
    if (Index >= 0 && !CallVirtual<bool>(this, Index, PlayerState))
    {
        return false;
    }
    // 4.2 0xA19780: bAllowRespawning (an unreflected flag OnPlaylistDataLoaded, 0xA1BE40, sets when the loaded playlist's
    // RespawnType isn't None), then true unless the playlist is InfiniteRespawnExceptStorm and the player died in the
    // storm. The playlist's RespawnType stands in for the flag: Core can't read it before 6.01.
    AFortGameStateAthena* GameState = AFortGameStateAthena::HasCurrentPlaylist() && UFortPlaylistAthena::HasRespawnType() ? AthenaGameStateOf(this) : nullptr;
    UFortPlaylistAthena* Playlist = GameState ? GameState->GetCurrentPlaylist() : nullptr;
    if (!Playlist || Playlist->RespawnType == EAthenaRespawnType::GetNone())
    {
        // From 6.01 the build's own yes stands: a mutator's (Playlist_Disco has no RespawnType).
        return Index >= 0;
    }
    // The storm rule: Core's reading of the death cause, from 6.01 on top of the build's own (the end of 6.02 0xBD08A0),
    // which is the death's location against the safe zone as it is now (0xBD04A0: the game mode's SafeZoneIndicator, its
    // place and Radius), in the safe zones phase, and only for a death the game stored: DeathInfo.bInitialized. Core's
    // OnPawnDied stores a player's death that way from 6.01 (the player state's own store, 0xC4A090), so the build's
    // own no was asked above, and it is the answer the pawn's death handler goes by too (0xC486D0 calls the build's
    // function, not this one): a player the storm killed outside the circle isn't respawned by the game any more while
    // Core has them out. The cause's rule stays under it for a death that isn't stored (a build before 6.01, a store
    // that wasn't found) and for a storm death the build's own would let back: that the storm's damage and 0xBD04A0
    // always agree on who is outside isn't something the image shows (the damage isn't applied through 0xBD04A0, whose
    // other caller is a pickup's despawn, 0xFC30A0), and a player who died to the storm must be out or a Playground
    // match never ends. Where the two disagree the game still respawns a player Core has out.
    return !PlayerState || !AFortPlayerStateAthena::HasEliminator() || Playlist->RespawnType != EAthenaRespawnType::GetInfiniteRespawnExceptStorm()
        || PlayerState->DeathInfo.DeathCause != EDeathCause::GetOutsideSafeZone();
}

float AFortGameModeAthena::GetRespawnHeight()
{
    // The playlist's RespawnHeight is a scalable float over its own game data (6.00's Playlist_Playground: Value 1 times
    // the row Playground.RespawnHeight, 15000), read at level 0 as the playlist's other rows are. A playlist without the
    // property (before 5.41) keeps TeleportToSkyDive's 10000, and so does one whose value is no height to skydive from:
    // TeleportToSkyDive's own rule (6.00 0xC9D2C0: any height up to 1000 is 10000), which also covers an unset one (0)
    // and a Value left without its row.
    constexpr float MinRespawnHeight = 1000.0f;
    AFortGameStateAthena* GameState = AFortGameStateAthena::HasCurrentPlaylist() && UFortPlaylistAthena::HasRespawnHeight() ? AthenaGameStateOf(this) : nullptr;
    UFortPlaylistAthena* Playlist = GameState ? GameState->GetCurrentPlaylist() : nullptr;
    if (!Playlist)
    {
        return DefaultRespawnHeight;
    }
    const FScalableFloat& PlaylistHeight = Playlist->RespawnHeight;
    const float Height = PlaylistHeight.GetValueAtLevel(0.0f);
    return Height > MinRespawnHeight ? Height : DefaultRespawnHeight;
}

bool AFortGameModeAthena::InitializeFlightPath()
{
#if WITH_SERVER_CODE
    // OnPlaylistDataLoaded's (4.2 0xA1BE40): if (!bFlightPathInitialized && MapInfo->IsMapInfoValidForAircraft())
    // { MapInfo->InitializeFlightPath(GameState); bFlightPathInitialized = true; }, which 3.5.1 to 4.1 also have in
    // StartWarmupPhase and StartAircraftPhase (Finder::ResolveFlightPathLive leaves it to them there).
    if (!HasFlightPathInitialized() || bFlightPathInitialized || !Finder::ResolveFlightPathLive(Member_bFlightPathInitialized.GetOffsetChecked()))
    {
        return false;
    }
    AFortGameStateAthena* GameState = AthenaGameStateOf(this);
    AFortAthenaMapInfo* MapInfo = GameState ? static_cast<AFortAthenaMapInfo*>(GameState->MapInfo) : nullptr;
    if (!MapInfo || !Natives::AFortAthenaMapInfo_IsMapInfoValidForAircraft(MapInfo))
    {
        Log::Warning(L"Battle Royale: the aircraft paths can't be built (map info {})", MapInfo ? L"not valid for an aircraft" : L"missing");
        return false;
    }
    Natives::AFortAthenaMapInfo_InitializeFlightPath(MapInfo, GameState);
    bFlightPathInitialized = true;
    return true;
#else
    return false;
#endif
}

void AFortGameModeAthena::ApplyPlaylistDataTablesOnce()
{
#if WITH_SERVER_CODE
    // The tables are global: a match before this world's left its playlist's rows in them until the next assignment, so
    // this world's first rolls would have been that playlist's.
    if (TablesGameMode.Get() == this || !Finder::ResolvePlaylistDataTablesLive())
    {
        return;
    }
    TablesGameMode = this;
    TablesPlaylistId = AppliedPlaylistId(this);
    TakeDefaultLootRows();
    Natives::AFortGameModeAthena_ApplyPlaylistDataTables(this);
    if (TablesPlaylistId >= 0)
    {
        MergeDefaultLootRows(TablesPlaylistId);
    }
    TablesLoot = PlaylistLootTables(this, TablesPlaylistId);
    const auto [LootTierData, LootPackages] = TablesLoot;
    Log::Info(L"Battle Royale: the world's loot tables are {} ({}, {})", TablesPlaylistId >= 0 ? std::format(L"playlist {}'s", TablesPlaylistId)
        : std::wstring(L"the defaults until the game state has the match's playlist"), LootTierData ? LootTierData->GetPathName().ToStdString() : std::wstring(L"default loot tiers"),
        LootPackages ? LootPackages->GetPathName().ToStdString() : std::wstring(L"default loot packages"));
#endif
}

void AFortGameModeAthena::SpawnVendingMachines()
{
#if WITH_SERVER_CODE
    // Whether a machine stands, its rarity, cost and what it sells are the match's playlist's: the rarity weights and
    // costs are AthenaGameData's Default.VendingMachine rows and the items its Loot_AthenaVending rows, which
    // ApplyPlaylistDataTables (3.5.1 0x9DC890) replaces with the playlist's own (Snipers, High Explosives and Sky Supply
    // have no machines, Solid Gold only Legendary ones, Playground its own weights and costs). From 3.5 the game state
    // has that playlist only from the match's assignment on, so the machines rolled at the world's creation took the
    // default tables in every LTM (#291). They wait for it (HandleCurrentPlaylistLoaded), or for the bus if it never comes.
    // A playlist without vending loot of its own (Blitz) gets the default rows back (MergeDefaultLootRows).
    ApplyPlaylistDataTablesOnce();
    if (TablesGameMode.Get() == this && TablesPlaylistId < 0 && AFortAthenaMapInfo::StaticClass() && AFortAthenaMapInfo::HasVendingMachines())
    {
        VendingGameMode = this;
        Log::Info(L"Battle Royale: the vending machines wait for the match's playlist");
        return;
    }
    SpawnMapVendingMachines(this);
#endif
}

namespace
{
    // How often the game phase is compared where GamePhaseChanged can't call Core (CheckGamePhase).
    constexpr float GamePhaseCheckInterval = 1.0f;

    // The match's pawns, the players' and the bots', tell the game mode when they die (AFortPawn::OnDied): a player's from
    // their Possess (OnPlayerPossessedPawn), a bot's once one of the match's events finds it with a pawn. Each is bound
    // once (AddUniqueDynamic).
    void BindMatchPawns(AFortGameModeAthena* Athena)
    {
        AFortGameStateAthena* GameState = PawnDiedHandler ? AthenaGameStateOf(Athena) : nullptr;
        if (!GameState)
        {
            return;
        }
        const FName Handler = PawnDiedHandler->GetFName();
        for (APlayerState* PlayerState : static_cast<const TArray<APlayerState*>&>(GameState->PlayerArray))
        {
            AController* Controller = PlayerState ? Cast<AController>(reinterpret_cast<UObject*>(PlayerState->GetOwner())) : nullptr;
            AFortPawn* Pawn = Controller ? Cast<AFortPawn>(reinterpret_cast<UObject*>(static_cast<APawn*>(Controller->Pawn))) : nullptr;
            if (Pawn)
            {
                FMulticastScriptDelegate& OnDied = Pawn->OnDied;
                OnDied.AddUniqueDynamic(Athena, Handler);
                // And when it is destroyed (HandlePawnDestroyed's log line).
                if (PawnDestroyedHandler)
                {
                    FMulticastScriptDelegate& OnDestroyed = Pawn->OnDestroyed;
                    OnDestroyed.AddUniqueDynamic(Athena, PawnDestroyedHandler->GetFName());
                }
            }
        }
    }

    // The tick after a pawn of the match died or a player left, the game's death handler or Logout done: the places
    // they gave, and a player the game moved to another team (the switch kills the pawn).
    void CheckMatchPlayers(AFortGameModeAthena* Athena)
    {
        MatchEventsOf(Athena).PlayersTimer.Invalidate();
        if (FMatchResults* Results = MatchResultsOf(Athena))
        {
            CheckMatchResults(Athena, *Results);
        }
        CheckTeamSwitches(Athena);
        BindMatchPawns(Athena);
    }

    // How long each of the match's storm circles holds and shrinks for, as the map info's definitions read now (the
    // match's playlist's game data): what StartNewSafeZonePhase sets the storm circle's times from.
    void ReportSafeZoneDefinitions(AFortGameModeAthena* Athena)
    {
        AFortGameStateAthena* GameState = AFortAthenaMapInfo::HasSafeZoneDefinitions() ? AthenaGameStateOf(Athena) : nullptr;
        AFortAthenaMapInfo* MapInfo = GameState ? static_cast<AFortAthenaMapInfo*>(GameState->MapInfo) : nullptr;
        if (!MapInfo)
        {
            return;
        }
        const TStructArray<FFortSafeZoneDefinition>& Definitions = MapInfo->SafeZoneDefinitions;
        std::wstring Circles;
        for (int32_t Index = 0; Index < Definitions.Num(); ++Index)
        {
            const FFortSafeZoneDefinition& Definition = Definitions[Index];
            Circles += std::format(L"{}{}: {:.0f}+{:.0f} (radius {:.0f})", Index ? L", " : L"", Index, Definition.WaitTime.GetValueAtLevel(0.0f),
                Definition.ShrinkTime.GetValueAtLevel(0.0f), Definition.Radius.GetValueAtLevel(0.0f));
        }
        Log::Info(L"Battle Royale: the storm's circles hold and shrink for, in seconds, {}; the first comes {:.0f} s after the safe zones start", Circles,
            MapInfo->SafeZoneStartDelay.GetValueAtLevel(0.0f));
    }

    // The game phase the game state is in, when it isn't the one handled last: the bus under way (the chests' and ammo
    // boxes' spawn percents, the Supply Llamas, the vending machines still waiting for the match's playlist), the
    // match's playlist by the warmup, the warmup's report, room and length, and the match's results.
    void RunGamePhase(AFortGameModeAthena* Athena)
    {
        FMatchEvents& Events = MatchEventsOf(Athena);
        AFortGameStateAthena* GameState = AthenaGameStateOf(Athena);
        const uint8_t Phase = GameState ? static_cast<uint8_t>(GameState->GamePhase) : Events.HandledPhase;
        if (Phase == Events.HandledPhase)
        {
            return;
        }
        Events.HandledPhase = Phase;
        // The storm's circles as the match's playlist has them, once the bus is under way and again as the storm starts.
        if (Phase == EAthenaGamePhase::GetAircraft() || Phase == EAthenaGamePhase::GetSafeZones())
        {
            ReportSafeZoneDefinitions(Athena);
        }

        // The bus is under way: the match's playlist is final, and nobody is near the map's containers.
        if (Phase >= EAthenaGamePhase::GetAircraft() && !Events.bAircraftHandled)
        {
            Events.bAircraftHandled = true;
            if (Events.bSpawnPercentsWaitForAircraft)
            {
                Events.bSpawnPercentsWaitForAircraft = false;
                ApplySpawnPercents(Athena);
            }
            SpawnMapLlamas(Athena);
            ClearGameModeTimer(Athena, Events.PlaylistTimer);
            SpawnWaitingVendingMachines(Athena, L"from the default tables: the bus is under way and the game state has no playlist");
        }
        else if (Phase == EAthenaGamePhase::GetWarmup())
        {
            // The match is assigned before its players come: its playlist is the world's by the warmup, whatever told
            // Core.
            ApplyMatchPlaylist(Athena);
        }

        WarmupPhaseChanged(Athena);
        if (FMatchResults* Results = MatchResultsOf(Athena))
        {
            CheckMatchResults(Athena, *Results);
        }
        CheckTeamSwitches(Athena);
        BindMatchPawns(Athena);
    }

    // The tick after the game state's GamePhaseChanged.
    void AfterGamePhaseChanged(AFortGameModeAthena* Athena)
    {
        MatchEventsOf(Athena).PhaseTimer.Invalidate();
        RunGamePhase(Athena);
    }

    // Where the build has no GamePhaseChanged, or Core couldn't add its native to the game mode's class (no
    // UClass::FuncMap): the game state's phase is compared every GamePhaseCheckInterval, and without the pawns' OnDied
    // the match's results and team switches are looked at then too. Epic's way is the broadcast BindMatchEvents binds.
    void CheckGamePhase(AFortGameModeAthena* Athena)
    {
        FMatchEvents& Events = MatchEventsOf(Athena);
        Events.PhaseTimer.Invalidate();
        RunGamePhase(Athena);
        if (!Events.bPawnsBound)
        {
            if (FMatchResults* Results = MatchResultsOf(Athena))
            {
                CheckMatchResults(Athena, *Results);
            }
            CheckTeamSwitches(Athena);
        }
        SetGameModeTimer(Athena, Events.PhaseTimer, GamePhaseCheckInterval, &CheckGamePhase);
    }
}

void AFortGameModeAthena::BindMatchEvents()
{
#if WITH_SERVER_CODE
    FMatchEvents& Events = MatchEventsOf(this);
    AFortGameStateAthena* GameState = AthenaGameStateOf(this);
    Events.HandledPhase = GameState ? static_cast<uint8_t>(GameState->GamePhase) : 0xFF;
    Events.bPawnsBound = PawnDiedHandler != nullptr;

    if (GameState && GamePhaseChangedHandler && AFortGameStateAthena::HasGamePhaseChanged())
    {
        FMulticastScriptDelegate& GamePhaseChanged = GameState->GamePhaseChanged;
        GamePhaseChanged.AddUniqueDynamic(this, GamePhaseChangedHandler->GetFName());
        Events.bPhaseBound = true;
    }
    else
    {
        Log::Warning(L"Battle Royale: the game state's GamePhaseChanged can't call Core ({}), so the game phase is looked at every {:.0f} s",
            !GameState ? L"no game state" : !AFortGameStateAthena::HasGamePhaseChanged() ? L"not in this build" : L"Core's handler couldn't be added", GamePhaseCheckInterval);
        SetGameModeTimer(this, Events.PhaseTimer, GamePhaseCheckInterval, &CheckGamePhase);
    }
    // The timer under the minimap: a log line at each of the game state's steps and of the storm's circles.
    if (GameState && GamePhaseStepChangedHandler && AFortGameStateAthena::HasGamePhaseStep())
    {
        FMulticastScriptDelegate& GamePhaseStepChanged = GameState->GamePhaseStepChanged;
        GamePhaseStepChanged.AddUniqueDynamic(this, GamePhaseStepChangedHandler->GetFName());
    }
    if (SafeZoneUpdatedHandler && HasSafeZoneUpdated())
    {
        FMulticastScriptDelegate& SafeZoneUpdated = OnSafeZoneUpdated;
        SafeZoneUpdated.AddUniqueDynamic(this, SafeZoneUpdatedHandler->GetFName());
    }

    // The match's playlist, once its assignment gives the world another one (ApplyPlaylistDataTablesOnce took the map
    // URL's): from 4.4 when it has loaded, before that looked for (CheckMatchPlaylist).
    const bool bPlaylistTables = TablesGameMode.Get() == this;
    const bool bPlaylistLoaded = bPlaylistTables && GameState && CurrentPlaylistLoadedHandler && AFortGameStateAthena::HasCurrentPlaylistLoaded();
    if (bPlaylistLoaded)
    {
        FMulticastScriptDelegate& OnCurrentPlaylistLoaded = GameState->OnCurrentPlaylistLoaded;
        OnCurrentPlaylistLoaded.AddUniqueDynamic(this, CurrentPlaylistLoadedHandler->GetFName());
    }
    else if (bPlaylistTables)
    {
        CheckMatchPlaylist(this);
    }
    Log::Info(L"Battle Royale: the match runs from the game state's GamePhaseChanged{}{}{}", Events.bPhaseBound ? L"" : L" (looked at)",
        bPlaylistLoaded ? L", its OnCurrentPlaylistLoaded" : bPlaylistTables ? L", the match's playlist looked for until the warmup ends" : L"",
        PawnDiedHandler ? L", the pawns' OnDied and the player states' OnDestroyed" : L", the match's results and team switches looked at with the phase");
#endif
}

void AFortGameModeAthena::HandleGamePhaseChanged()
{
#if WITH_SERVER_CODE
    FMatchEvents& Events = MatchEventsOf(this);
    if (!Events.PhaseTimer.IsValid())
    {
        SetGameModeTimer(this, Events.PhaseTimer, NextTickDelay, &AfterGamePhaseChanged);
    }
#endif
}

void AFortGameModeAthena::HandleGamePhaseStepChanged()
{
#if WITH_SERVER_CODE
    ReportGamePhaseStep(L"the game state's step changed");
#endif
}

void AFortGameModeAthena::HandleSafeZoneUpdated()
{
#if WITH_SERVER_CODE
    ReportGamePhaseStep(L"a new storm circle");
#endif
}

void AFortGameModeAthena::ReportGamePhaseStep(const wchar_t* Event)
{
#if WITH_SERVER_CODE
    AFortGameStateAthena* GameState = AthenaGameStateOf(this);
    UWorld* World = GetWorld();
    if (!GameState || !World || !AFortGameStateAthena::HasGamePhaseStep())
    {
        return;
    }
    const float Now = World->GetTimeSeconds();
    // A time against now: "12 s ahead", "3 s ago".
    const auto Against = [Now](float Time) { return Time >= Now ? std::format(L"{:.0f} s ahead", Time - Now) : std::format(L"{:.0f} s ago", Now - Time); };

    // The storm circle: the game mode's own, and the one the game state replicates, which the clients' timers read.
    AFortSafeZoneIndicator* Indicator = static_cast<AFortSafeZoneIndicator*>(SafeZoneIndicator);
    AFortSafeZoneIndicator* Replicated = AFortGameStateAthena::HasSafeZoneIndicator() ? static_cast<AFortSafeZoneIndicator*>(GameState->SafeZoneIndicator) : Indicator;
    std::wstring Circle = L"no storm circle yet";
    if (Indicator)
    {
        AFortAthenaMapInfo* MapInfo = AFortAthenaMapInfo::HasSafeZoneDefinitions() ? static_cast<AFortAthenaMapInfo*>(GameState->MapInfo) : nullptr;
        Circle = std::format(L"storm circle {}{}", static_cast<int32_t>(SafeZonePhase), MapInfo ? std::format(L" of {}", MapInfo->SafeZoneDefinitions.Num()) : std::wstring());
        if (AFortSafeZoneIndicator::HasShrinkTimes())
        {
            const float Start = Indicator->SafeZoneStartShrinkTime;
            const float Finish = Indicator->SafeZoneFinishShrinkTime;
            Circle += std::format(L", it starts to shrink {} and has shrunk {}", Against(Start), Against(Finish));
        }
        if (AFortSafeZoneIndicator::HasNextCenter())
        {
            Circle += std::format(L", radius {:.0f} to {:.0f}", static_cast<float>(Indicator->Radius), static_cast<float>(Indicator->NextRadius));
        }
        if (AFortSafeZoneIndicator::HasPausedForPreview() && Indicator->bPausedForPreview)
        {
            Circle += L", only shown for now (bPausedForPreview)";
        }
        if (HasSafeZoneActive() && bSafeZonePaused)
        {
            Circle += L", its phases held (bSafeZonePaused)";
        }
        Circle += Replicated == Indicator ? L"; the game state replicates it" : Replicated ? L"; the game state replicates ANOTHER one" : L"; the game state replicates NONE";
        // Which clients have it: a client without an actor channel for it has no storm circle at all (#412: nothing on
        // its map, and its timer at 0:00 from the bus on). A circle spawned this frame has none yet.
        if (UNetDriver* Driver = Natives::UNetConnection_ActorChannels >= 0 ? World->GetNetDriver() : nullptr)
        {
            int32_t Open = 0;
            int32_t WithChannel = 0;
            for (UNetConnection* Connection : static_cast<const TArray<UNetConnection*>&>(Driver->ClientConnections))
            {
                if (Connection && Connection->State == USOCK_Open)
                {
                    ++Open;
                    WithChannel += Connection->ActorChannels.FindRef(Indicator) ? 1 : 0;
                }
            }
            Circle += std::format(L", and {} of {} clients have a channel for it", WithChannel, Open);
        }
    }
    if (AFortGameStateAthena::HasSafeZonesStartTime())
    {
        Circle += std::format(L"; SafeZonesStartTime {}", Against(GameState->SafeZonesStartTime));
    }
    if (AFortGameStateAthena::HasCountdowns() && (GameState->bIsInCountdown || GameState->bIsInFinalCountdown))
    {
        Circle += GameState->bIsInFinalCountdown ? L"; the playlist's final countdown runs" : L"; the playlist's countdown runs";
    }

    // The bus's drop times, which the aircraft phase's steps are.
    std::wstring Bus;
    if (static_cast<uint8_t>(GameState->GamePhase) == EAthenaGamePhase::GetAircraft() && AFortGameStateAthena::HasAircrafts() && AFortAthenaAircraft::HasDropTimes())
    {
        for (AFortAthenaAircraft* Aircraft : static_cast<const TArray<AFortAthenaAircraft*>&>(GameState->Aircrafts))
        {
            if (Aircraft)
            {
                Bus += std::format(L"; a bus's drop starts {} and ends {}", Against(Aircraft->DropStartTime), Against(Aircraft->DropEndTime));
            }
        }
    }

    UFortPlaylistAthena* Playlist = AFortGameStateAthena::HasCurrentPlaylist() ? GameState->GetCurrentPlaylist() : nullptr;
    const std::wstring StartUp = Playlist && UFortPlaylistAthena::HasSafeZoneStartUp()
        ? std::format(L"; {}'s SafeZoneStartUp {}", static_cast<FName>(Playlist->PlaylistName).ToStdString(), static_cast<int32_t>(static_cast<uint8_t>(Playlist->SafeZoneStartUp))) : std::wstring();
    Log::Info(L"Battle Royale: the timer's step is {} with {:.0f} s left ({}), phase {}, at {:.1f} s: {}{}{}", GameState->GamePhaseStep.ToString(),
        static_cast<float>(GameState->GamePhaseStepTimeRemaining), Event, GameState->GamePhase.ToString(), Now, Circle, Bus, StartUp);
#endif
}

void AFortGameModeAthena::HandleCurrentPlaylistLoaded()
{
#if WITH_SERVER_CODE
    // The game state broadcasts it right after the playlist's modifiers registered (OnPlaylistDataLoadCompleted, 6.01
    // 0xC1C6A0, after UpdatePlaylistDependentData), so their mutators have taken the match's property overrides.
    if (AFortGameStateAthena* GameState = AthenaGameStateOf(this))
    {
        GameState->ReportPropertyOverrides();
        // The mutators the playlist's modifiers spawned, as the game mode and the game state heard of them: the lists
        // the game's own code asks (a death's respawn, Disco Domination's score and dance floors). Nothing to say on a
        // playlist without mutators.
        const int32_t ModeMutators = GameplayMutatorsOffset() >= 0 ? GetMember_GameplayMutators().Num() : 0;
        const int32_t StateMutators = AFortGameStateAthena::GameplayMutatorsOffset() >= 0 ? GameState->GetMember_GameplayMutators().Num() : 0;
        if (ModeMutators > 0 || StateMutators > 0)
        {
            UFortPlaylistAthena* Playlist = GameState->GetCurrentPlaylist();
            Log::Info(L"PLAYLIST: {}'s mutators: {} answer for the game mode, {} for the game state{}", Playlist ? static_cast<FName>(Playlist->PlaylistName).ToStdString() : std::wstring(L"the playlist"),
                ModeMutators, StateMutators, AFortGameStateAthena::HasAllowRespawning() ? (GameState->bAllowRespawning ? L"; bAllowRespawning is set" : L"; bAllowRespawning isn't set") : L"");
        }
    }
    // A playlist that brings its players back says how far over their death once, as it loads (#411).
    if (IsRespawningAllowed(nullptr))
    {
        Log::Info(L"Battle Royale: the match's playlist brings its players back {:.0f} over where they died ({})", GetRespawnHeight(),
            UFortPlaylistAthena::HasRespawnHeight() ? L"the playlist's RespawnHeight, or 10000 where that is no height to skydive from" : L"this build's playlists have no RespawnHeight");
    }
    FMatchEvents& Events = MatchEventsOf(this);
    if (!Events.PlaylistTimer.IsValid())
    {
        SetGameModeTimer(this, Events.PlaylistTimer, NextTickDelay, &AfterCurrentPlaylistLoaded);
    }
#endif
}

void AFortGameModeAthena::HandlePawnDied()
{
#if WITH_SERVER_CODE
    FMatchEvents& Events = MatchEventsOf(this);
    if (!Events.PlayersTimer.IsValid())
    {
        SetGameModeTimer(this, Events.PlayersTimer, NextTickDelay, &CheckMatchPlayers);
    }
#endif
}

void AFortGameModeAthena::HandlePlayerStateDestroyed(AActor* DestroyedActor)
{
#if WITH_SERVER_CODE
    // The player's place as they leave: a player who leaves the match alive is out of it (AFortGameModeAthena's Logout
    // takes them out of the alive players, RemoveFromAlivePlayers), and their team's place comes with it when they were
    // its last. Their player state has left the game state's PlayerArray by now (APlayerState::Destroyed), so it is read
    // here, and the match's results are looked at the tick after.
    AFortPlayerStateAthena* State = Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(DestroyedActor));
    FMatchResults* Results = State ? MatchResultsOf(this) : nullptr;
    if (!Results)
    {
        return;
    }
    if (FMatchPlayer* Player = UpdateMatchPlayer(*Results, State))
    {
        Player->Kills = State->GetKills();
        Player->BotKills = std::min(FCoreAthenaBotManager::BotKillsOf(State), std::max(Player->Kills, 0));
    }
    HandlePawnDied();
#endif
}

void AFortGameModeAthena::HandlePawnDestroyed(AActor* DestroyedActor)
{
#if WITH_SERVER_CODE
    // A dead pawn's only: the warmup pawns the aircraft phase destroys, alive, would be a line for every player at once.
    AFortPawn* Pawn = Cast<AFortPawn>(reinterpret_cast<UObject*>(DestroyedActor));
    if (!Pawn)
    {
        return;
    }
    const bool bTornOff = Pawn->bTearOff;
    const bool bDying = Pawn->bIsDying;
    if (bTornOff || bDying)
    {
        Log::Info(L"Battle Royale: the dead pawn {} (0x{:X}) is destroyed, {}", Pawn->GetName().ToStdString(), reinterpret_cast<uintptr_t>(Pawn), bTornOff ? L"torn off" : L"not torn off");
    }
#endif
}

void AFortGameModeAthena::OnPlayerPossessedPawn(AFortPlayerController* Player, APawn* Pawn)
{
#if WITH_SERVER_CODE
    if (!Player)
    {
        return;
    }
    if (AFortPawn* FortPawn = PawnDiedHandler ? Cast<AFortPawn>(reinterpret_cast<UObject*>(Pawn)) : nullptr)
    {
        FMulticastScriptDelegate& OnDied = FortPawn->OnDied;
        OnDied.AddUniqueDynamic(this, PawnDiedHandler->GetFName());
        if (PawnDestroyedHandler)
        {
            FMulticastScriptDelegate& PawnOnDestroyed = FortPawn->OnDestroyed;
            PawnOnDestroyed.AddUniqueDynamic(this, PawnDestroyedHandler->GetFName());
        }
    }
    if (AActor* PlayerState = PlayerStateDestroyedHandler ? reinterpret_cast<AActor*>(static_cast<APlayerState*>(Player->PlayerState)) : nullptr)
    {
        FMulticastScriptDelegate& OnDestroyed = PlayerState->OnDestroyed;
        OnDestroyed.AddUniqueDynamic(this, PlayerStateDestroyedHandler->GetFName());
    }
    CheckTeamSwitches(this);
#endif
}

DEFINE_FUNCTION(AFortGameModeAthena::execHandleGamePhaseChanged)
{
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<AFortGameModeAthena*>(Context)->HandleGamePhaseChanged();
    P_NATIVE_END;
}

DEFINE_FUNCTION(AFortGameModeAthena::execHandleGamePhaseStepChanged)
{
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<AFortGameModeAthena*>(Context)->HandleGamePhaseStepChanged();
    P_NATIVE_END;
}

DEFINE_FUNCTION(AFortGameModeAthena::execHandleSafeZoneUpdated)
{
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<AFortGameModeAthena*>(Context)->HandleSafeZoneUpdated();
    P_NATIVE_END;
}

DEFINE_FUNCTION(AFortGameModeAthena::execHandleCurrentPlaylistLoaded)
{
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<AFortGameModeAthena*>(Context)->HandleCurrentPlaylistLoaded();
    P_NATIVE_END;
}

DEFINE_FUNCTION(AFortGameModeAthena::execHandlePawnDied)
{
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<AFortGameModeAthena*>(Context)->HandlePawnDied();
    P_NATIVE_END;
}

DEFINE_FUNCTION(AFortGameModeAthena::execHandlePlayerStateDestroyed)
{
    P_GET_OBJECT(AActor, DestroyedActor);
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<AFortGameModeAthena*>(Context)->HandlePlayerStateDestroyed(DestroyedActor);
    P_NATIVE_END;
}

DEFINE_FUNCTION(AFortGameModeAthena::execHandlePawnDestroyed)
{
    P_GET_OBJECT(AActor, DestroyedActor);
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<AFortGameModeAthena*>(Context)->HandlePawnDestroyed(DestroyedActor);
    P_NATIVE_END;
}

void AFortGameModeAthena::AddDelegateHandlers()
{
    // The game's broadcasts call each bound UFunction through ProcessEvent (ProcessMulticastDelegate, 5.40 0x2ECA90), so
    // Core's natives go on the Battle Royale game mode's class, the object they are bound with, with each delegate's own
    // parameters.
    UClass* AthenaClass = StaticClass();
    if (!AthenaClass)
    {
        return;
    }
    const auto Add = [AthenaClass](const wchar_t* Name, FNativeFuncPtr Thunk, UClass* DelegateClass, const wchar_t* DelegateName) -> UFunction*
    {
        const UFunction* Signature = DelegateSignature(DelegateClass, DelegateName);
        return Signature ? AthenaClass->AddNativeFunction(Name, Thunk, Signature) : nullptr;
    };
    GamePhaseChangedHandler = Add(L"HandleGamePhaseChanged", &execHandleGamePhaseChanged, AFortGameStateAthena::StaticClass(), L"GamePhaseChanged");
    GamePhaseStepChangedHandler = Add(L"HandleGamePhaseStepChanged", &execHandleGamePhaseStepChanged, AFortGameStateAthena::StaticClass(), L"GamePhaseStepChanged");
    SafeZoneUpdatedHandler = Add(L"HandleSafeZoneUpdated", &execHandleSafeZoneUpdated, AthenaClass, L"OnSafeZoneUpdated");
    CurrentPlaylistLoadedHandler = Add(L"HandleCurrentPlaylistLoaded", &execHandleCurrentPlaylistLoaded, AFortGameStateAthena::StaticClass(), L"OnCurrentPlaylistLoaded");
    PawnDiedHandler = Add(L"HandlePawnDied", &execHandlePawnDied, AFortPawn::StaticClass(), L"OnDied");
    // Only with the pawns' OnDied: without it the results are looked at with the phase, the leaving player's among them.
    PlayerStateDestroyedHandler = PawnDiedHandler ? Add(L"HandlePlayerStateDestroyed", &execHandlePlayerStateDestroyed, AActor::StaticClass(), L"OnDestroyed") : nullptr;
    // The same delegate on the pawns themselves, bound where their OnDied is.
    PawnDestroyedHandler = PawnDiedHandler && AActor::HasOnDestroyed() ? Add(L"HandlePawnDestroyed", &execHandlePawnDestroyed, AActor::StaticClass(), L"OnDestroyed") : nullptr;
    Log::Info(L"Battle Royale: Core's handlers for GamePhaseChanged {}, OnCurrentPlaylistLoaded {}, OnDied {}, OnDestroyed {} (a pawn's {})", GamePhaseChangedHandler ? L"added" : L"none",
        CurrentPlaylistLoadedHandler ? L"added" : L"none", PawnDiedHandler ? L"added" : L"none", PlayerStateDestroyedHandler ? L"added" : L"none", PawnDestroyedHandler ? L"added" : L"none");
}

void AFortGameModeAthena::HookVTables()
{
    AddDelegateHandlers();
    // IsRespawningAllowed's own slot (from 6.01), on a Battle Royale server: only a build whose game state reflects
    // bAllowRespawning is looked at.
    if (Configuration::Get().IsBattleRoyale() && StaticClass() && AFortGameStateAthena::HasAllowRespawning())
    {
        Finder::ResolveIsRespawningAllowedLive(GameplayMutatorsOffset(), AFortGameStateAthena::AllowRespawningOffset(), AFortGameStateAthena::AllowRespawningMask());
    }

    UClass* AthenaClass = StaticClass();
    UObject* ZoneDefault = AthenaClass ? AFortGameModeZone::StaticClass()->GetDefaultObject(false) : nullptr;
    void** ZoneVTable = ZoneDefault ? *reinterpret_cast<void***>(ZoneDefault) : nullptr;
    if (!ZoneVTable || !Natives::AFortGameModeZone_SpawnDefaultPawnFor)
    {
        return;
    }

    // AFortGameModeAthena::SpawnDefaultPawnFor_Implementation (1.7.2 0x3FEE60) keeps its four "SpawnDefaultPawnFor:
    // Failed due to no ..." checks and returns null where it spawned the pawn, so nothing spawns: not on the warmup island,
    // not on a warmup respawn, not when jumping from the aircraft or at its forced drop (all RestartPlayer). Its slot is the
    // Zone mode's; the Zone mode's restoration (the checks, then AFortGameMode's spawn at the start spot) is the same, apart
    // from the aircraft (SpawnDefaultPawnFor_Implementation).
    // The first slot holding it: vtables sit back to back (no RTTI), so the next class's repeats it, which
    // Finder::VTableIndex counts as ambiguous.
    int32_t SpawnIndex = -1;
    for (int32_t Index = 0; Index < 0x200 && SpawnIndex < 0; ++Index)
    {
        SpawnIndex = ZoneVTable[Index] == reinterpret_cast<void*>(Natives::AFortGameModeZone_SpawnDefaultPawnFor) ? Index : -1;
    }
    if (SpawnIndex < 0)
    {
        Log::Error(L"Finder: AFortGameModeZone::SpawnDefaultPawnFor_Implementation's slot not found, Battle Royale players won't spawn");
        return;
    }
    for (TObjectIterator<UClass> It; SpawnIndex >= 0 && It; ++It)
    {
        UClass* Class = *It;
        UObject* ClassDefault = Class->IsChildOf(AthenaClass) ? Class->GetDefaultObject(false) : nullptr;
        void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
        if (VTable && VTable[SpawnIndex] != reinterpret_cast<void*>(&SpawnDefaultPawnFor_ImplementationHK))
        {
            Hooks::SwapVTable(L"AFortGameModeAthena::SpawnDefaultPawnFor_Implementation", VTable, SpawnIndex, reinterpret_cast<void*>(&SpawnDefaultPawnFor_ImplementationHK));
        }
    }
}

APawn* AFortGameModeAthena::SpawnDefaultPawnFor_ImplementationHK(AFortGameModeAthena* This, AController* NewPlayer, AActor* StartSpot)
{
    return This->SpawnDefaultPawnFor_Implementation(NewPlayer, StartSpot);
}

void AFortGameModeAthena::HookCallSites()
{
    // StartAircraftPhase's per-player cleanup after the warmup pawn is destroyed is stripped (a call to the shared empty
    // function), so the warmup island's loot went onto the aircraft (#86). 2.2 compiles in the call it took on 1.8.1 and
    // later (AFortPlayerController::ActivateSlot), which removes nothing, so the loot went onto the aircraft again (#190).
    if (uint8_t* Call = Finder::AircraftPhasePlayerCleanupCall(AFortPlayerController::QuickBarsOffset()))
    {
        Hooks::RedirectCall(L"AFortGameModeAthena::StartAircraftPhase player cleanup", Call, reinterpret_cast<void*>(&AircraftPhasePlayerCleanupCall));
    }
}

void AFortGameModeAthena::AircraftPhasePlayerCleanupCall(AController* Controller, uint8_t InQuickBar, int32_t Slot, float ActivateDelay, bool bUpdatePreviousFocusedSlot)
{
    AFortPlayerControllerAthena* PlayerController = Cast<AFortPlayerControllerAthena>(reinterpret_cast<UObject*>(Controller));

    // A player boarding in build mode keeps it on their client (#222). The client leaves build mode only when its own
    // quickbar's focus moves off the building bar (FocusSlot, 2.5 0xC5D9D0, pops BuildPreviewModeInputComponent there
    // through 0xE1C490); a focus moved on the server (the pickaxe slot's activation below, or Possess's when the player
    // leaves the aircraft) reaches the client as replicated values, which never run it. Its ClientRestart's
    // RefocusQuickBars (0xE2DBC0) only acts while the building bar still has the focus, so the player landed with the
    // pickaxe in hand and the build preview out. AFortPlayerController's ExecuteInventoryItem (0xE0B390), which a slot
    // activation runs, has the client equip the slot's item through ClientExecuteInventoryItem, and the client focuses it
    // on its own quickbar (0xDFC050 -> 0xC5DDB0); Athena's (0x999F30) executes it on the server only. Send it for the
    // harvesting tool now, forced as the warmup pawn is on its way out, before the server's focus changes (an RPC goes
    // out at once, the quickbars replicate at the end of the tick).
    AFortQuickBars* Bars = PlayerController ? static_cast<AFortQuickBars*>(PlayerController->QuickBars) : nullptr;
    if (Bars && Bars->GetCurrentQuickBar() == EFortQuickBars::GetSecondary())
    {
        FQuickBar& PrimaryQuickBar = Bars->PrimaryQuickBar;
        TStructArray<FQuickBarSlot>& Slots = PrimaryQuickBar.Slots;
        const TArray<FGuid>* SlotItems = nullptr;
        if (Slots.Num() > 0)
        {
            const TArray<FGuid>& HarvestingToolSlotItems = Slots[0].Items;
            SlotItems = &HarvestingToolSlotItems;
        }
        if (SlotItems && SlotItems->Num() > 0)
        {
            PlayerController->ClientExecuteInventoryItem((*SlotItems)[0], 0.0f, true);
            Log::Info(L"Battle Royale: {} boards the aircraft in build mode, its client takes the harvesting tool", PlayerController->GetName().ToStdString());
        }
    }

    if (PlayerController)
    {
        PlayerController->ResetToStartingItems();
    }

    // Where the call was ActivateSlot's (2.2), the harvesting tool's slot is still activated, once the warmup items are gone.
    if (Natives::AFortPlayerController_ActivateSlot)
    {
        Natives::AFortPlayerController_ActivateSlot(reinterpret_cast<AFortPlayerController*>(Controller), InQuickBar, Slot, ActivateDelay, bUpdatePreviousFocusedSlot);
    }
}

void AFortGameModeAthena::HookBuildingVTables()
{
    UClass* AthenaClass = StaticClass();
    UObject* AthenaDefault = AthenaClass && Finder::ResolveScoreBuildingConstructionLive() ? AthenaClass->GetDefaultObject(false) : nullptr;
    void** AthenaVTable = AthenaDefault ? *reinterpret_cast<void***>(AthenaDefault) : nullptr;
    if (!AthenaVTable)
    {
        return;
    }

    // Each Battle Royale game mode class whose slot is the Athena game mode's own (AFortGameMode's, 1.7.2 0x589F50).
    const int32_t Index = Natives::AFortGameMode_ScoreBuildingConstructionIndex;
    void* const Score = AthenaVTable[Index];
    ScoreBuildingConstructionOG = reinterpret_cast<decltype(ScoreBuildingConstructionOG)>(Score);
    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Class = *It;
        UObject* ClassDefault = Class->IsChildOf(AthenaClass) ? Class->GetDefaultObject(false) : nullptr;
        void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
        if (VTable && VTable[Index] == Score)
        {
            Hooks::SwapVTable(L"AFortGameModeAthena::ScoreBuildingConstruction", VTable, Index, reinterpret_cast<void*>(&ScoreBuildingConstructionHK));
        }
    }
}

void AFortGameModeAthena::ScoreBuildingConstructionHK(AFortGameModeAthena* This, AController* Builder, ABuildingSMActor* Building)
{
#if WITH_SERVER_CODE
    // A built piece takes its builder's team through the builder's WorldPlayerId: ServerCreateBuildingActor (1.7.2
    // 0x825810, 1.8 0x845D70) initializes the piece with it (0x80DAD0, slot 0xF7), and a piece still on its constructor's
    // Spectator takes the team of the player state with that id (0x4333B0, 1.8 0x47B730: WorldPlayerId +0x540, then
    // SetTeam, slot 0x117). The id stays -1 for every Battle Royale player (Core gives it in its Zone Login hook, which
    // Athena's own Login, 0x3F2450, never reaches, and Battle Royale has no world record to give it from), so every piece
    // stayed Spectator, and CanBePlayerEdited (0x461EF0, 1.8 0x4646F0) only compares the editor's team with a piece's
    // that isn't: anyone could edit anyone's walls (#120). The game mode scores the piece right after (0x825E72, 1.8
    // 0x8463D2) with its builder, whose team is the one CanBePlayerEdited reads for the editor (the controller's team is
    // its player state's, 0x80CE10, Athena's is TeamIndex, 0x42FE70). An edit's new piece copies the old one's team
    // (ReplaceBuildingActor, 0x47D5D0). Save the World's game modes keep their slot.
    AFortPlayerStateAthena* PlayerState = Builder ? Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(static_cast<APlayerState*>(Builder->PlayerState))) : nullptr;
    if (Building && Building->Role == ROLE_Authority && Building->Team == EFortTeam::GetSpectator())
    {
        if (PlayerState)
        {
            Building->Team = PlayerState->TeamIndex;
        }
        else
        {
            Log::Warning(L"Battle Royale: {} built by {} has no team, anyone can edit it", Building->GetName().ToStdString(), Builder ? Builder->GetName().ToStdString() : L"nobody");
        }
    }
#endif
    ScoreBuildingConstructionOG(This, Builder, Building);
}

void AFortGameModeAthena::OnGivenMatchAssignment(bool bSuccess, const void* MatchOptions)
{
#if WITH_SERVER_CODE
    // 5.21's (0xBD59E0) takes the matchmaking service's assignment from the game session's delegate (5.30 binds it at
    // 0xBE7BF4; the game session broadcasts it once the service has closed its socket, 0x101E8E0). 5.30 strips it to the
    // shared empty function, so the game state never got the match's playlist: nothing loaded it
    // (LoadCurrentPlaylistData), the game mode's OnPlaylistDataLoaded never ran, the match stayed in its Setup phase and
    // its players on their loading screen (#367).
    if (!bSuccess)
    {
        return;
    }
    // 5.21 first sends the "MatchStarted" analytics event (0xBE1B40), which Core doesn't.
    if (Natives::AFortGameMode_OnGivenMatchAssignment)
    {
        Natives::AFortGameMode_OnGivenMatchAssignment(this, bSuccess, MatchOptions);
    }

    // 5.21's game session gave the game mode the assignment's playlist before this (CurrentPlaylistId, or its name when
    // the id is 0; 0xFF7C90). 5.30's assignment names the playlist (HandleAssignMatchMessage, 0x6F2350, reads the sixth
    // of its bucket's ":" fields as an FName, "PlaylistName: %s") and its game session sets only CurrentBucketId and
    // MatchmakingRegionId from it (0x101DC90), so the name is read from that bucket the same way and set with the game's
    // own SetCurrentPlaylistName (0xE40340).
    const std::wstring Bucket = static_cast<FString&>(CurrentBucketId).ToStdString();
    std::vector<std::wstring> Fields;
    for (size_t Begin = 0; Begin <= Bucket.size();)
    {
        const size_t End = std::min(Bucket.find(L':', Begin), Bucket.size());
        if (End > Begin)
        {
            Fields.push_back(Bucket.substr(Begin, End - Begin));
        }
        Begin = End + 1;
    }
    if (Fields.size() >= 6 && Natives::AFortGameMode_SetCurrentPlaylistName)
    {
        Natives::AFortGameMode_SetCurrentPlaylistName(this, FName(Fields[5].c_str()));
    }
    const FName PlaylistName = CurrentPlaylistName;
    const std::wstring PlaylistText = PlaylistName.IsNone() ? std::to_wstring(static_cast<int32_t>(CurrentPlaylistId)) : PlaylistName.ToStdString();
    Log::Info(L"PLAYLIST: Assignment Received in AFortGameModeAthena::OnGivenMatchAssignment() PlaylistName is {} (Server-Only)", PlaylistText);

    // The game state's playlist (5.21's AFortGameStateAthena::SetCurrentPlaylistId, inlined there): the playlist of that
    // name, else of the game mode's id, unless the game state has it already, loaded or loading.
    if (AFortGameStateAthena* GameState = AthenaGameStateOf(this))
    {
        if (!PlaylistName.IsNone())
        {
            GameState->SetCurrentPlaylistId(PlaylistName);
        }
        else
        {
            GameState->SetCurrentPlaylistId(static_cast<int32_t>(CurrentPlaylistId));
        }
    }

    // The match is the players the assignment reserved (configuring the beacon left the game session's MaxPlayers at
    // them), and its warmup waits for a tenth of them. Not with -nomcp (nor in the editor).
    AGameSession* Session = static_cast<AGameSession*>(GameSession);
    if (Session && AGameSession::HasMaxPlayers() && HasMaxPlayerCount() && !FParse::Param(FCommandLine::Get(), TEXT("nomcp")))
    {
        const uint8_t Teams = static_cast<uint8_t>(Session->MaxPlayers - Session->MaxSpectators);
        NumTeams = Teams;
        MaxPlayerCount = Teams;
        WarmupRequiredPlayerCount = std::max(1, static_cast<int32_t>(std::floor(static_cast<float>(Teams) * 0.1f + 0.5f)));
    }
    // 5.21 then starts the failed assignment timer, whose handler ("FortGameModeAthena: Failed match assignment,
    // restarting session.") 5.30 strips too, names the positional voice channel (confctl-d-%s), which Core's voice chat
    // doesn't use, and keeps the match options' isMatchStatsDisabled, isProgressionDisabled and custom key in flags the
    // build doesn't reflect (AFortGameMode's own took the custom key above).
    Log::Info(L"AFortGameModeAthena::OnGivenMatchAssignment: MaxPlayerCount: {}, NumTeams: {}, WarmupRequiredPlayerCount: {}, Timeout: {:.2f}",
        static_cast<int32_t>(MaxPlayerCount), static_cast<int32_t>(static_cast<uint8_t>(NumTeams)), static_cast<int32_t>(WarmupRequiredPlayerCount),
        static_cast<float>(FailedMatchAssignmentTimeout));
#endif
}

void AFortGameModeAthena::HookMatchAssignmentVTables()
{
    UClass* AthenaClass = StaticClass();
    UObject* GameModeDefault = AthenaClass ? AFortGameMode::StaticClass()->GetDefaultObject(false) : nullptr;
    void** GameModeVTable = GameModeDefault ? *reinterpret_cast<void***>(GameModeDefault) : nullptr;
    if (!GameModeVTable || !Natives::SharedEmptyFunction || !Finder::ResolveMatchAssignmentLive(GameModeVTable))
    {
        return;
    }
    // Only where the Battle Royale game mode's own is stripped (5.30); 5.21's and earlier keep theirs.
    const int32_t Index = Natives::AFortGameMode_OnGivenMatchAssignmentIndex;
    int32_t Swapped = 0;
    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Class = *It;
        UObject* ClassDefault = Class->IsChildOf(AthenaClass) ? Class->GetDefaultObject(false) : nullptr;
        void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
        if (VTable && VTable[Index] == Natives::SharedEmptyFunction)
        {
            Hooks::SwapVTable(L"AFortGameModeAthena::OnGivenMatchAssignment", VTable, Index, reinterpret_cast<void*>(&OnGivenMatchAssignmentHK));
            ++Swapped;
        }
    }
    if (Swapped > 0)
    {
        Log::Info(L"Finder: AFortGameModeAthena::OnGivenMatchAssignment is stripped, restored in {} game mode classes", Swapped);
    }
}

void AFortGameModeAthena::OnGivenMatchAssignmentHK(AFortGameModeAthena* This, bool bSuccess, const void* MatchOptions)
{
    This->OnGivenMatchAssignment(bSuccess, MatchOptions);
}
