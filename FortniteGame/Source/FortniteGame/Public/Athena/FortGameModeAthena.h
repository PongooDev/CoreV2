#pragma once

#include "FortGameModeZone.h"
#include "UObject/Stack.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

class AController;
class AFortSafeZoneIndicator;
class AActor;
class ABuildingSMActor;
class AFortPlayerController;
class AFortPlayerControllerAthena;
class AFortPlayerStateAthena;
class AFortPawn;
class APawn;
class UObject;
struct FFortPlayerDeathReport;
class UDataTable;

// A playlist's data tables (from 1.11; Athena_GameMode_C's PlaylistDataTables): the tables whose rows
// ApplyPlaylistDataTables copies over the default ones (AthenaLootTierData_Client, AthenaLootPackages_Client and the rest)
// while the match's playlist is PlaylistId. From 3.0 the entry is only FortAthenaPlaylistClass, an AFortAthenaPlaylistBase
// class whose default object has the PlaylistId and the tables.
struct FAthenaPlaylistDataTableSet
{
    DefineUnrealStruct(FAthenaPlaylistDataTableSet, FortniteGame)
    DefineUProperty(int32_t, PlaylistId)
    DefineUProperty(UDataTable*, LootTierData)
    DefineUProperty(UDataTable*, LootPackages)
    DefineUProperty(UClass*, FortAthenaPlaylistClass)

    static bool HasPlaylistId() { return Member_PlaylistId.IsAvailable(); }
    static bool HasFortAthenaPlaylistClass() { return Member_FortAthenaPlaylistClass.IsAvailable(); }
};

// Battle Royale's game mode (from 1.7.2; Athena_GameMode_C on Athena_Terrain). Its flow (warmup, aircraft, safe zones,
// end game) is native; what the shipping exe stripped from it is restored here (Core-Docs Athena-Gameplay-1.7.2.md).
class AFortGameModeAthena : public AFortGameModeZone
{
    DefineUnrealClass(AFortGameModeAthena, FortniteGame)

public:
    DefineUProperty(int32_t, SafeZonePhase)
    DefineUProperty(AFortSafeZoneIndicator*, SafeZoneIndicator)
    // Each safe zone's center, picked when the storm starts (index: the phase).
    DefineUProperty(TArray<FVector>, SafeZoneLocations)
    // Up to 4.2; 4.4 reads its playlist's MaxTeamSize instead.
    DefineUProperty(bool, bSquadPlay)
    static bool HasSquadPlay() { return Member_bSquadPlay.IsAvailable(); }
    // Whether a dead player goes on to spectate (SpectateOnDeath).
    DefineUProperty(bool, bAllowSpectateAfterDeath)
    // Who PreLogin lets in ("Match is full" at it, 1.9 0x4319F0): the assigned players (OnGivenMatchAssignment,
    // 0x42D2B0).
    DefineUProperty(int32_t, MaxPlayerCount)
    // The warmup's length, and the shorter one it changes to once enough of MaxPlayerCount is in (UpdateWarmupPhase,
    // 1.9 0x43D9A0). Config values; no match option sets them.
    DefineUProperty(float, WarmupCountdownDuration)
    DefineUProperty(float, WarmupEarlyCountdownDuration)
    // Whether the map info's FlightInfos (one aircraft path, or one per team with OpposingAirCraftForEachTeam) are built:
    // OnPlaylistDataLoaded (4.1 0xA2F500) and StartAircraftPhase (0xA3B530) build them (AFortAthenaMapInfo, 0xA2B8F0)
    // only while it is false, and set it. 4.2 builds them only in OnPlaylistDataLoaded (0xA1BE40).
    DefineUProperty(bool, bFlightPathInitialized)
    static bool HasFlightPathInitialized() { return Member_bFlightPathInitialized.IsAvailable(); }
    // Where StartAircraftPhase no longer builds the aircraft paths (4.2): OnPlaylistDataLoaded's build of them, while
    // bFlightPathInitialized is false and the map info is valid for an aircraft (AFortAthenaMapInfo::
    // InitializeFlightPath with the game state). For a match whose playlist never loads (the bots-only test match), and
    // when the match's playlist replaces the world's. True when it built them.
    bool InitializeFlightPath();
    // 1.11 to 3.3; from 3.5 the playlist is a data asset, AFortGameStateAthena's CurrentPlaylistData.
    DefineUStructArrayProperty(FAthenaPlaylistDataTableSet, PlaylistDataTables)
    static bool HasPlaylistDataTables() { return Member_PlaylistDataTables.IsAvailable(); }
    // The players still in the match (AddToAlivePlayers adds them, RemoveFromAlivePlayers takes them off).
    DefineUProperty(TArray<AFortPlayerControllerAthena*>, AlivePlayers)
    // The alive players on each team (3.2's int32 TeamAlivePlayers[100], index: the team less HumanPvP_Team1), as
    // AddToAlivePlayers and RemoveFromAlivePlayers count them; TeamsLeft follows its teams above 0.
    DefineUProperty(int32_t, TeamAlivePlayers)
    static bool HasTeamAlivePlayers() { return Member_TeamAlivePlayers.IsAvailable(); }
    int32_t GetTeamAlivePlayers(int32_t TeamIndex) { return TeamIndex >= 0 && TeamIndex < 100 ? (&GetMember_TeamAlivePlayers())[TeamIndex] : 0; }
    // The playlist's (3.2's Teams of 20: teams of several squads), and 50v50's squads. A large
    // team game's new pawns have bTeamMembersAlwaysRelevant (FinishRestartPlayer, 3.2 0x9B6310), unless it is 50v50.
    DefineUProperty(bool, bIsLargeTeamGame)
    DefineUProperty(bool, bFiftyFiftySquadPlay)
    static bool HasLargeTeamGame() { return Member_bIsLargeTeamGame.IsAvailable() && Member_bFiftyFiftySquadPlay.IsAvailable(); }

    static bool HasMaxPlayerCount() { return Member_MaxPlayerCount.IsAvailable(); }
    // AFortGamePvPBase's teams, the players the warmup waits for before its countdown, how long an assignment may take
    // (config) and the match's AI (UAthenaAISettings), which OnGivenMatchAssignment sets.
    DefineUProperty(uint8_t, NumTeams)
    DefineUProperty(int32_t, WarmupRequiredPlayerCount)
    DefineUProperty(float, FailedMatchAssignmentTimeout)
    DefineUProperty(UObject*, AISettings)
    static bool HasAISettings() { return Member_AISettings.IsAvailable(); }

    // The matchmaking service's assignment of a match to the server (the game session's delegate, on its vtable slot):
    // AFortGameMode's (the match options), then the game state's playlist, the match's player count and its warmup's
    // required players. 5.30 strips it (Core's restoration; 5.21 0xBD59E0 is the game's own).
    void OnGivenMatchAssignment(bool bSuccess, const void* MatchOptions);

    // The most players a match here can have (the playlist's teams x their size; 0 when unknown), and the players
    // connected now (from their login on). Battle Royale's player bots fill the difference (FCoreAthenaBotManager).
    int32_t GetMatchCapacity();
    int32_t GetNumConnectedPlayers();
    // The playlist's team size and number of teams (the reservation beacon's NumPlayersPerTeam and NumTeams; 0 without one).
    int32_t GetPlayersPerTeam();
    int32_t GetNumTeams();
    // The players the match has places for, by team: each reservation's players (the reservation beacon's, the assigned
    // parties' and the late ones Core reserved) with their EFortTeam (HumanPvP_Team1 for beacon team 0) and account id,
    // those still loading in included. Empty without a beacon.
    std::vector<std::pair<uint8_t, std::wstring>> GetTeamReservations();
    static bool HasWarmupLength() { return Member_WarmupCountdownDuration.IsAvailable() && Member_WarmupEarlyCountdownDuration.IsAvailable(); }

    // Where a Battle Royale server starts: the Battle Royale zone theme's map and playlist
    // ("/Game/Athena/Maps/Athena_Terrain?playlistId=2" on 1.7.2), or empty when the build has none.
    static FString DefaultMatchURL();

    // The world's game mode when it is Battle Royale's, else null (Save the World and the builds before Battle Royale).
    static AFortGameModeAthena* GetCurrent(const UObject* WorldContextObject);

    // A dedicated server's Battle Royale session is AFortGameSessionDedicatedAthena from 5.0, the session that registers
    // with the matchmaking service; before 5.0 AFortGameMode's (AFortGameSessionDedicated) does.
    TSubclassOf<AGameSession> GetGameSessionClass() const;

    // The loot tier group Battle Royale rolls in place of a container's or building's own (None: no loot).
    FName RedirectLootTierGroup(FName TierGroup);

    // Adds the match's playlist tag (Athena.Playlist.Solo, Duo, Squad or FiftyFifty, with its parents) to Tags unless
    // it is there, and says whether it added it.
    bool AddPlaylistContextTag(FGameplayTagContainer& Tags);

    APawn* SpawnDefaultPawnFor_Implementation(AController* NewPlayer, AActor* StartSpot);

    // 4.2 0xA19780: whether the match's playlist brings its players back after they die (EAthenaRespawnType), and with
    // InfiniteRespawnExceptStorm not one the storm killed (PlayerState's DeathInfo). The pawn's death handler (4.2
    // AFortPlayerPawnAthena 0xA81B50, after its controller's) then has the controller's RespawnPlayerAfterDeath run once
    // the death penalty's respawn countdown is over, and ChoosePlayerStart (0xA0AE70) and PlayerCanRestart (0xA1E6F0)
    // let it restart in the safe zones phase. False before 3.5.
    bool IsRespawningAllowed(AFortPlayerStateAthena* PlayerState);
    // How far over where they died a respawning player (or player bot) starts skydiving: the match's playlist's
    // RespawnHeight where the playlist has one (from 5.41) and it comes to more than 1000, else DefaultRespawnHeight. The
    // 21.40 exports name a GetRespawnHeight beside IsRespawningAllowed; its class and signature aren't in them, and the
    // builds' own reader went with the stripped server code.
    float GetRespawnHeight();
    // TeleportToSkyDive's height above a spot (AFortPlayerPawnAthena 4.2 0xA98690: 10000 for any height up to 1000).
    static constexpr float DefaultRespawnHeight = 10000.0f;

    // Battle Royale's changes to a dead player's death report, after AFortPlayerControllerZone::PrepareDeathReport
    // filled it (AFortPlayerControllerAthena's override calls it; 1.9.1 and 1.10 strip it).
    void ModifyDeathReport(AFortPlayerControllerAthena* Player, FFortPlayerDeathReport& DeathReport);

    // Every player still in the match outlived the one who is out (a player, or a player bot, CoreV2 #233): an
    // EFortQuestObjectiveStatEvent::AthenaOutlive event for each, as 1.8's game mode sends one per death (0x599DA0).
    void SendOutliveStatEvents(AFortPawn* DeadPawn);

    // Removes the placed chests and ammo boxes this match doesn't keep, from the map info's spawn percents (#205): once the
    // bus is under way on builds whose playlists bring their own game data (from 1.11, #238), at once before them.
    void ApplyContainerSpawnPercents();

    // Has the map info roll the placed vending machines' rarities and what they sell (AFortAthenaMapInfo::
    // SpawnVendingMachines, from 3.5, #257) with the match's playlist's tables: at once where they are already in place,
    // else once the game state has that playlist (HandleCurrentPlaylistLoaded, #291) or the bus is under way.
    void SpawnVendingMachines();

    // Has the world's BGA consumable spawners (ABGAConsumableSpawner, from 4.0's Hop Rocks, #278) roll and place their
    // building gameplay actors, with the world's loot tables. Builds without them do nothing.
    void SpawnBGAConsumables();

    // Binds the match's events Core works from (FinishWorldInitialization, once the game state is there): the game
    // state's GamePhaseChanged (HandleGamePhaseChanged) and, from 4.4, its OnCurrentPlaylistLoaded
    // (HandleCurrentPlaylistLoaded). Each player's pawn and player state are bound as the player gets a pawn
    // (OnPlayerPossessedPawn).
    void BindMatchEvents();

    // The game state's GamePhaseChanged (from 1.7.2; AFortGameStateAthena::OnRep_GamePhase broadcasts it, and the server's
    // SetGamePhase calls that on each change: 1.8.1 0x4251A0, 3.2 0x9BD5B0, 5.40 0xBEDB40): the phase-driven work, the
    // tick after (outside the game mode's StartWarmupPhase or StartAircraftPhase). When the bus is under way the chests' and ammo boxes' spawn percents (ApplyContainerSpawnPercents) and the Supply
    // Llamas (AFortAthenaMapInfo::SpawnLlamas, from 3.3, #243), whose call went with the stripped server code, and the
    // vending machines still waiting for the match's playlist; the warmup's report, room and length (StartWarmupReports);
    // and the match's results (StartMatchResults).
    void HandleGamePhaseChanged();

    // From 4.4: the game state's OnCurrentPlaylistLoaded, which AFortGameStateAthena::OnPlaylistDataLoadCompleted
    // (5.40 0xBF32A0, "Server AND Client") broadcasts after the game mode's OnPlaylistDataLoaded. Once the match's
    // assignment gives the world another playlist, applies that playlist's tables (from 3.5 with the default loot rows
    // of the tier groups and packages they have none of, #292) and, when its loot tables are other ones, rolls the
    // world's loot again, as long as the match hasn't left its warmup.
    void HandleCurrentPlaylistLoaded();

    // A pawn of the match died (AFortPawn::OnDied, which the pawn's death broadcasts on the server; Epic's own
    // AFortTeamInfo::AddSpottedActor binds it too): its team may be out, and the game may have moved its player to
    // another team. Looked at the tick after, once the death handler has given the places.
    void HandlePawnDied();

    // A player's player state is going away (AActor::OnDestroyed: APlayerState::Destroyed, after the game mode's Logout
    // has taken the player out of the match): the player's place, kills and time alive as they leave.
    void HandlePlayerStateDestroyed(AActor* DestroyedActor);

    // A player's pawn is destroyed (AActor::OnDestroyed, which AActor::Destroyed broadcasts): a dead one's name and
    // address go to the log, beside the address of each pawn a respawn spawns, so that a log shows whether a new pawn
    // took a dead pawn's address (#411: the replication graph keeps each connection's per-actor info by the actor's
    // address, a dead pawn's with its tear-off flag).
    void HandlePawnDestroyed(AActor* DestroyedActor);

    // AFortPlayerController::Possess on the server: the player's pawn and player state tell the match when the pawn dies
    // or is destroyed and when the player leaves, and a player the game moved to another team comes back in its squad.
    void OnPlayerPossessedPawn(AFortPlayerController* Player, APawn* Pawn);

    // Reports each player's place, kills and time alive to Core-Backend once their team is out of the match or has won,
    // for Battle Royale's stats (Core-Backend #59): looked at when a pawn of the match dies, a player leaves and the game
    // phase changes.
    void StartMatchResults();

    // Keeps the warmup at the admin panel's length and, with late joining on, takes more players during it
    // (Core-Backend #122): reports to the backend from the world's start, again each time it answers and at each phase
    // change, until the bus has left.
    void StartWarmupReports();

    // Has the server and its players fetch the event calendar again (AFortGameState::Client_RefreshEventCalendar) as soon
    // as the admin panel changes it (Core-Backend's calendar revision), so that a running match sees an admin's event
    // change (4.5's rocket launch) at once, not at the calendar's cacheExpire. Asks again a while after each answer.
    // Builds without the function do nothing.
    void AskEventCalendarRevision();

    // The game's own ApplyPlaylistDataTables (from 1.11), once per game mode, before the world's first loot roll: it
    // restores the default loot tables and copies the playlist's over them (the playlist the map URL gave, until the
    // match is assigned). Builds without it keep their tables.
    void ApplyPlaylistDataTablesOnce();

    static void HookVTables();
    static void HookCallSites();
    static void HookBuildingVTables();
    // Restores OnGivenMatchAssignment in the Battle Royale game modes whose slot is the shared empty function (5.30).
    static void HookMatchAssignmentVTables();

private:
    // Core's natives the game's Blueprint delegates call (UClass::AddNativeFunction at boot, with each delegate's
    // signature): GamePhaseChanged, OnCurrentPlaylistLoaded, a pawn's OnDied and OnDestroyed and a player state's
    // OnDestroyed, all bound to the game mode. None where the build has no such delegate or Core can't add a native.
    static void AddDelegateHandlers();
    DECLARE_FUNCTION(execHandleGamePhaseChanged);
    DECLARE_FUNCTION(execHandleCurrentPlaylistLoaded);
    DECLARE_FUNCTION(execHandlePawnDied);
    DECLARE_FUNCTION(execHandlePlayerStateDestroyed);
    DECLARE_FUNCTION(execHandlePawnDestroyed);

    static void OnGivenMatchAssignmentHK(AFortGameModeAthena* This, bool bSuccess, const void* MatchOptions);
    static APawn* SpawnDefaultPawnFor_ImplementationHK(AFortGameModeAthena* This, AController* NewPlayer, AActor* StartSpot);
    // The arguments are AFortPlayerController::ActivateSlot's; the 1.7.2 call it also takes passes only the controller.
    static void AircraftPhasePlayerCleanupCall(AController* Controller, uint8_t InQuickBar, int32_t Slot, float ActivateDelay, bool bUpdatePreviousFocusedSlot);
    static inline void (*ScoreBuildingConstructionOG)(AFortGameModeAthena* This, AController* Builder, ABuildingSMActor* Building) = nullptr;
    static void ScoreBuildingConstructionHK(AFortGameModeAthena* This, AController* Builder, ABuildingSMActor* Building);
};
