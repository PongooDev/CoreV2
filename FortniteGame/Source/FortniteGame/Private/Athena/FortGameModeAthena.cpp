#include "pch.h"
#include "Athena/FortGameModeAthena.h"
#include "Athena/AI/CoreAthenaBotManager.h"
#include "Athena/FortAthenaAircraft.h"
#include "Athena/FortAthenaMapInfo.h"
#include "Athena/FortAthenaPlaylistBase.h"
#include "Athena/FortPlaylistAthena.h"
#include "Building/BGAConsumableSpawner.h"
#include "Building/BuildingContainer.h"
#include "Items/FortLootPackage.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Athena/FortGameStateAthena.h"
#include "Athena/FortPlayerControllerAthena.h"
#include "Athena/FortPlayerStateAthena.h"
#include "BackendRequest.h"
#include "Building/BuildingSMActor.h"
#include "Configuration.h"
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
#include "Math/Rotator.h"
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
#include "UObject/UObjectGlobals.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"
#include "UObject/WeakObjectPtr.h"

#include <algorithm>
#include <cctype>
#include <chrono>
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

    // Runs Then once the bus is under way, the match's playlist final by then.
    void WhenAircraftPhase(FWeakObjectPtr GameMode, void (*Then)(AFortGameModeAthena*))
    {
        GameThread::Enqueue([GameMode, Then]()
        {
            AFortGameModeAthena* Athena = static_cast<AFortGameModeAthena*>(GameMode.Get());
            UWorld* World = Athena ? Athena->GetWorld() : nullptr;
            if (!World)
            {
                return;
            }
            AFortGameStateAthena* GameState = Cast<AFortGameStateAthena>(reinterpret_cast<UObject*>(World->GetGameState()));
            if (GameState && GameState->GamePhase >= EAthenaGamePhase::GetAircraft())
            {
                Then(Athena);
                return;
            }
            WhenAircraftPhase(GameMode, Then);
        });
    }
}

void AFortGameModeAthena::WatchLlamas()
{
#if WITH_SERVER_CODE
    // Core's: Epic's call to AFortAthenaMapInfo::SpawnLlamas went with the stripped server code. 3.3 keeps no call to it
    // (no call to the shared empty function has the map info as this), so it can't be redirected. The llamas are placed
    // when the bus is under way, the match's playlist final by then: their quantity rows are AthenaGameData's, which a
    // playlist's own game data replaces (as for the spawn percents), and the match's playlist comes with its assignment.
    if (AFortAthenaMapInfo::HasLlamas())
    {
        WhenAircraftPhase(FWeakObjectPtr(this), [](AFortGameModeAthena* Athena)
        {
            UWorld* World = Athena->GetWorld();
            AFortGameStateAthena* GameState = World ? Cast<AFortGameStateAthena>(reinterpret_cast<UObject*>(World->GetGameState())) : nullptr;
            if (AFortAthenaMapInfo* MapInfo = GameState ? static_cast<AFortAthenaMapInfo*>(GameState->MapInfo) : nullptr)
            {
                MapInfo->SpawnLlamas();
            }
        });
    }
#endif
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
    // so on those builds the percents wait for the bus, when the playlist is the match's and its game data in place.
    // Nobody is near the map's containers then (the bus flies at AthenaGameData's Default.Aircraft.Height, 80000 on 3.2).
    if (Finder::ResolvePlaylistDataTablesLive())
    {
        WhenAircraftPhase(FWeakObjectPtr(this), &ApplySpawnPercents);
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
    // where the player died, at TeleportToSkyDive's height (0xA98690), and skydives from there.
    FVector RespawnLocation;
    bool bRespawnInAir = false;
    if (!Aircraft && StartSpot && NewPlayer && FDeathInfo::HasDeathLocation() && AFortPlayerStateAthena::HasEliminator())
    {
        UWorld* SpotWorld = GetWorld();
        AFortGameStateAthena* GameState = SpotWorld ? Cast<AFortGameStateAthena>(reinterpret_cast<UObject*>(SpotWorld->GetGameState())) : nullptr;
        AFortPlayerStateAthena* AthenaState = Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(static_cast<APlayerState*>(NewPlayer->PlayerState)));
        if (GameState && AthenaState && GameState->GamePhase == EAthenaGamePhase::GetSafeZones() && IsRespawningAllowed(AthenaState))
        {
            const FVector DeathLocation = AthenaState->DeathInfo.DeathLocation;
            bRespawnInAir = DeathLocation.X != 0.0f || DeathLocation.Y != 0.0f || DeathLocation.Z != 0.0f;
            RespawnLocation = DeathLocation + FVector(0.0f, 0.0f, RespawnHeight);
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
            Log::Info(L"Battle Royale: {} respawns in the air as {} at ({:.0f}, {:.0f}, {:.0f})", NewPlayer->GetName().ToStdString(), Pawn->GetName().ToStdString(),
                RespawnLocation.X, RespawnLocation.Y, RespawnLocation.Z);
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
        std::map<std::wstring, FMatchPlayer> Players;
        std::chrono::steady_clock::time_point NextCheck;
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
            AFortPlayerStateAthena* AthenaState = Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(PlayerState));
            const std::wstring AccountId = AthenaState ? AthenaState->UniqueId.ToString().ToStdString() : std::wstring();
            if (!IsAccountId(AccountId))
            {
                continue;
            }
            FMatchPlayer& Player = Results.Players[AccountId];
            if (Player.bReported)
            {
                continue;
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

        // The end-of-match screen of each player still here whose place is final, its kills counted above
        // (AFortPlayerController::SendMatchStatsForPlayer sends it once).
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

    void WatchResults(FWeakObjectPtr GameMode, std::shared_ptr<FMatchResults> Results)
    {
        GameThread::Enqueue([GameMode, Results]()
        {
            AFortGameModeAthena* Athena = static_cast<AFortGameModeAthena*>(GameMode.Get());
            if (!Athena)
            {
                return;
            }
            const auto Now = std::chrono::steady_clock::now();
            if (Now >= Results->NextCheck)
            {
                Results->NextCheck = Now + std::chrono::seconds(1);
                CheckMatchResults(Athena, *Results);
            }
            WatchResults(GameMode, Results);
        });
    }
}

void AFortGameModeAthena::WatchMatchResults()
{
#if WITH_SERVER_CODE
    // Epic's stats service kept Battle Royale's stats (Balance/DataTables/LeaderboardStats: br_placetop<n>, br_kills,
    // br_matchesplayed, br_minutesplayed per playlist) from what its servers reported, but the 1.7.2 server reports only
    // its winners (battleroyale/recordwin, 0x72E130): UFortMcpUtils::RecordUserStatsUrl (/api/stats/bulk?ownertype=1)
    // has no caller left, so wins were the only stat. The rest comes from the player states: Place is set for every
    // player of a team when the team is out (TeamsLeft then, 0x44A870, from the death handler 0x437C30) or has won (1,
    // 0x400490 and 0x4008F0), with SecondsAlive and the player's Kills final by then. They are read once a second and
    // each player is reported once, with the DedicatedServer client's token. Core-Backend counts the win from recordwin,
    // which 1.8 doesn't have (Core-Backend #81): from a build without it, the report says to count a place 1 as the win.
    auto Results = std::make_shared<FMatchResults>();
    Results->bCountWins = Finder::StringRefs(L"/api/game/v2/battleroyale/recordwin").empty();
    const FGuid MatchGuid = FGuid::NewGuid();
    Results->MatchId = std::format("{:08X}{:08X}{:08X}{:08X}", MatchGuid.A, MatchGuid.B, MatchGuid.C, MatchGuid.D);
    WatchResults(FWeakObjectPtr(this), Results);
    Log::Info(L"Battle Royale: reporting the match's results to {} ({}{})", Configuration::Get().Backend,
        Results->bCountWins ? L"with its wins" : L"its wins through the build's recordwin",
        AFortPlayerStateAthena::CountsKills() ? L"" : AFortPlayerStateAthena::HasEliminator() ? L", kills counted by Core" : L", no kills: no DeathInfo");
#endif
}

namespace
{
    // How often the server asks for the admin panel's settings before its warmup, and reports during it.
    constexpr auto WarmupAskInterval = std::chrono::seconds(30);
    constexpr auto WarmupReportInterval = std::chrono::seconds(1);
    constexpr auto ReserveInterval = std::chrono::milliseconds(100);

    struct FWarmupWatch
    {
        std::chrono::steady_clock::time_point NextAsk{};
        std::chrono::steady_clock::time_point NextReserve{};
        bool bAsking = false;
        // The backend has no such route (one from before Core-Backend #122): it isn't asked again.
        bool bUnsupported = false;
        // The bus has left and the backend has heard so.
        bool bReportedEnd = false;
        // The phase the last check saw: a new one is reported at once.
        uint8_t LastPhase = 0xFF;

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

    AFortGameStateAthena* AthenaGameStateOf(AFortGameModeAthena* GameMode)
    {
        UWorld* World = GameMode->GetWorld();
        return World ? Cast<AFortGameStateAthena>(reinterpret_cast<UObject*>(World->GetGameState())) : nullptr;
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

    // The account ids of the players connected (from their login on, so those still loading the map count).
    std::vector<std::wstring> ConnectedAccounts(AFortGameModeAthena* GameMode)
    {
        std::vector<std::wstring> Ids;
        UWorld* World = GameMode->GetWorld();
        UNetDriver* Driver = World && UNetConnection::HasPlayerID() ? World->GetNetDriver() : nullptr;
        if (!Driver)
        {
            return Ids;
        }
        for (UNetConnection* Connection : static_cast<TArray<UNetConnection*>&>(Driver->ClientConnections))
        {
            const std::wstring Id = Connection && Connection->PlayerID.IsValid() ? Connection->PlayerID.ToString().ToStdString() : std::wstring();
            if (IsAccountId(Id))
            {
                Ids.push_back(Id);
            }
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
            FFortMcpConfig::DedicatedServerClientId, FFortMcpConfig::DedicatedServerClientSecret, [Watch](uint32_t Status, const std::string& Body)
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
            GameThread::Enqueue([Watch, Status, bAnswered, WarmupSeconds, bLateJoin, bOpen, MaxPlayers, Parties = std::move(Parties), bBots, BotSettings, NoFill = std::move(NoFill)]() mutable
            {
                Watch->bAsking = false;
                if (bAnswered)
                {
                    if (FCoreAthenaBotManager* BotManager = FCoreAthenaBotManager::Get())
                    {
                        BotManager->SetSettings(bBots, BotSettings);
                    }
                }
                if (Status == 404 && !Watch->bUnsupported)
                {
                    Watch->bUnsupported = true;
                    Log::Warning(L"Battle Royale: the backend has no warmup settings (Core-Backend #122), so the match keeps the game's warmup and takes no late players");
                }
                if (!bAnswered)
                {
                    return;
                }
                Watch->WarmupSeconds = WarmupSeconds;
                Watch->bAnswered = true;
                Watch->bLateJoin = bLateJoin;
                Watch->bOpen = bOpen;
                Watch->MaxPlayers = MaxPlayers;
                Watch->Parties = std::move(Parties);
                Watch->NoFill = std::set<std::wstring>(NoFill.begin(), NoFill.end());
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

    void CheckWarmup(AFortGameModeAthena* GameMode, const std::shared_ptr<FWarmupWatch>& Watch, std::chrono::steady_clock::time_point Now)
    {
        AFortGameStateAthena* GameState = AthenaGameStateOf(GameMode);
        UWorld* World = GameMode->GetWorld();
        if (!GameState || !World)
        {
            return;
        }
        const uint8_t Phase = GameState->GamePhase;
        const bool bWarmup = Phase == EAthenaGamePhase::GetWarmup();
        const float WorldNow = World->GetTimeSeconds();
        if (Phase != Watch->LastPhase)
        {
            Watch->LastPhase = Phase;
            Watch->NextAsk = Now;
        }

        KeepSessionRoom(GameMode, *Watch, Phase);
        if (Phase <= EAthenaGamePhase::GetWarmup())
        {
            ApplyWarmupLength(GameMode, GameState, *Watch, WorldNow);
        }
        if (bWarmup && Watch->bOpen)
        {
            OpenMatch(GameMode, *Watch);
        }
        if (Watch->bOpened && Now >= Watch->NextReserve)
        {
            Watch->NextReserve = Now + ReserveInterval;
            ReserveLatePlayers(GameMode, *Watch, bWarmup);
        }

        // Every 30 seconds before the warmup (the settings), every second during it (the match's players and time left),
        // once after.
        if (Watch->bAsking || Watch->bUnsupported || Watch->bReportedEnd || Now < Watch->NextAsk)
        {
            return;
        }
        const bool bAfter = Phase > EAthenaGamePhase::GetWarmup();
        Watch->NextAsk = Now + (bWarmup ? WarmupReportInterval : WarmupAskInterval);
        Watch->bReportedEnd = bAfter;
        float SecondsLeft = 0.0f;
        if (bWarmup && AFortGameStateAthena::HasWarmupCountdown())
        {
            SecondsLeft = GameState->WarmupCountdownEndTime >= 0.0f ? GameState->WarmupCountdownEndTime - WorldNow
                : AFortGameModeAthena::HasWarmupLength() ? GameMode->WarmupCountdownDuration : 0.0f;
        }
        if (bAfter)
        {
            Watch->bOpen = false;
        }
        ReportWarmup(Watch, GameState->GamePhase.ToString(), ConnectedAccounts(GameMode), SecondsLeft);
    }

    void WatchWarmupOf(FWeakObjectPtr GameMode, std::shared_ptr<FWarmupWatch> Watch)
    {
        GameThread::Enqueue([GameMode, Watch]()
        {
            AFortGameModeAthena* Athena = static_cast<AFortGameModeAthena*>(GameMode.Get());
            AFortGameStateAthena* GameState = Athena ? AthenaGameStateOf(Athena) : nullptr;
            // Ends with the match, or once the storm is up: everyone who came during the warmup is in the game by then.
            if (!Athena || (GameState && GameState->GamePhase > EAthenaGamePhase::GetAircraft() && Watch->bReportedEnd))
            {
                return;
            }
            CheckWarmup(Athena, Watch, std::chrono::steady_clock::now());
            WatchWarmupOf(GameMode, Watch);
        });
    }
}

void AFortGameModeAthena::WatchWarmup()
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
    WatchWarmupOf(FWeakObjectPtr(this), std::make_shared<FWarmupWatch>());
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
        return !AFortGameModeAthena::HasPlaylistDataTables() && AFortGameStateAthena::HasCurrentPlaylistData();
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
            AFortGameStateAthena* GameState = AFortGameStateAthena::HasCurrentPlaylistData() ? AthenaGameStateOf(GameMode) : nullptr;
            UFortPlaylistAthena* Playlist = GameState ? GameState->CurrentPlaylistData : nullptr;
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
        if (AFortGameModeAthena::HasPlaylistDataTables() || !AFortGameStateAthena::HasCurrentPlaylistData())
        {
            return GameMode->CurrentPlaylistId;
        }
        AFortGameStateAthena* GameState = AthenaGameStateOf(GameMode);
        UFortPlaylistAthena* Playlist = GameState ? static_cast<UFortPlaylistAthena*>(GameState->CurrentPlaylistData) : nullptr;
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

    void WatchPlaylistDataTablesOf(FWeakObjectPtr GameMode)
    {
        GameThread::Enqueue([GameMode]()
        {
            AFortGameModeAthena* Athena = static_cast<AFortGameModeAthena*>(GameMode.Get());
            AFortGameStateAthena* GameState = Athena ? AthenaGameStateOf(Athena) : nullptr;
            // The match is assigned before its players come, in the warmup at the latest.
            if (!Athena || TablesGameMode.Get() != Athena || !GameState)
            {
                return;
            }
            if (GameState->GamePhase >= EAthenaGamePhase::GetAircraft())
            {
                SpawnWaitingVendingMachines(Athena, L"from the default tables: the bus is under way and the game state has no playlist");
                return;
            }
            // The match's playlist is CurrentPlaylistId once the matchmaking service's assignment is being handled
            // (HandlingMatchAssignment), but OnGivenMatchAssignment, which ends with ApplyPlaylistDataTables, runs only
            // when the server has answered it (Finished), some 20 s later on 2.4.2. The tables are applied now, the same
            // way, so the loot is rolled again before the players come; OnGivenMatchAssignment's copy is the same rows.
            // From 3.5 the tables wait for the game state's playlist (AppliedPlaylistId).
            const int32_t PlaylistId = AppliedPlaylistId(Athena);
            if (PlaylistId != TablesPlaylistId && PlaylistId >= 0)
            {
                const int32_t Previous = TablesPlaylistId;
                TablesPlaylistId = PlaylistId;
                RebuildFlightPathsFor(Athena, PlaylistId, Previous);
                Natives::AFortGameModeAthena_ApplyPlaylistDataTables(Athena);
                MergeDefaultLootRows(PlaylistId);
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
            WatchPlaylistDataTablesOf(GameMode);
        });
    }
}

bool AFortGameModeAthena::IsRespawningAllowed(AFortPlayerStateAthena* PlayerState)
{
    // 4.2 0xA19780: bAllowRespawning (an unreflected flag OnPlaylistDataLoaded, 0xA1BE40, sets when the loaded playlist's
    // RespawnType isn't None), then true unless the playlist is InfiniteRespawnExceptStorm and the player died in the
    // storm. The playlist's RespawnType stands in for the flag: Core can't read it.
    AFortGameStateAthena* GameState = AFortGameStateAthena::HasCurrentPlaylistData() && UFortPlaylistAthena::HasRespawnType() ? AthenaGameStateOf(this) : nullptr;
    UFortPlaylistAthena* Playlist = GameState ? static_cast<UFortPlaylistAthena*>(GameState->CurrentPlaylistData) : nullptr;
    if (!Playlist || Playlist->RespawnType == EAthenaRespawnType::GetNone())
    {
        return false;
    }
    return !PlayerState || !AFortPlayerStateAthena::HasEliminator() || Playlist->RespawnType != EAthenaRespawnType::GetInfiniteRespawnExceptStorm()
        || PlayerState->DeathInfo.DeathCause != EDeathCause::GetOutsideSafeZone();
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
    // default tables in every LTM (#291). They wait for it (WatchPlaylistDataTables), or for the bus if it never comes.
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

void AFortGameModeAthena::WatchPlaylistDataTables()
{
#if WITH_SERVER_CODE
    if (TablesGameMode.Get() == this)
    {
        WatchPlaylistDataTablesOf(FWeakObjectPtr(this));
    }
#endif
}

void AFortGameModeAthena::HookVTables()
{
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
