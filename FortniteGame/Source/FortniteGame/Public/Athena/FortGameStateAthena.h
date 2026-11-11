#pragma once

#include "FortGameState.h"

class AFortAthenaAircraft;
class AFortAthenaMapInfo;
class UFortPlaylistAthena;

class EAthenaGamePhase
{
    DefineUnrealEnum(EAthenaGamePhase, FortniteGame)
    DefineUEnumValue(None)
    DefineUEnumValue(Setup)
    DefineUEnumValue(Warmup)
    DefineUEnumValue(Aircraft)
    DefineUEnumValue(SafeZones)
    DefineUEnumValue(EndGame)
};

// Battle Royale's game state (from 1.7.2; Athena_GameState_C).
class AFortGameStateAthena : public AFortGameStateZone
{
    DefineUnrealClass(AFortGameStateAthena, FortniteGame)

public:
    // Set by AFortGameModeAthena's phase changes. After Warmup a dead player is out of the match: the pawn-death handler
    // (1.7.2 0x437C30) only takes a player out of the alive players from Aircraft on. GamePhaseStep follows on its own,
    // on the server too: the game state's Tick (5.40 0xC061E0) works it out every frame from the phase, the warmup
    // countdown, the aircraft and the safe zone indicator's times (0xC06E10) and broadcasts GamePhaseStepChanged when it
    // changes (AFortGameModeAthena's and the mutators' OnGamePhaseStepChanged). Its gate, CanUpdateGamePhaseStep
    // (0xBE0640, 5.30 0xBD98A0), is false only while UFortGameInstance's loading screen manager (the TWeakPtr that
    // UFortGlobalUIContext::IsCurrentlyShowingLoadingScreen reads) shows one, so it is true on Core's servers.
    DefineUProperty(EAthenaGamePhase, GamePhase)
    DefineUProperty(int32_t, PlayersLeft)
    // The most players the match had: AddToAlivePlayers (1.10 0x42A4A0) raises it to the alive players' count.
    DefineUProperty(int32_t, TotalPlayers)
    static bool HasTotalPlayers() { return Member_TotalPlayers.IsAvailable(); }
    DefineUProperty(int32_t, TeamsLeft)
    // A large team game's players left on each team (index: the team less HumanPvP_Team1), which the HUD's
    // "<Team>{YourTeamAlive}</> : <EnemyTeam>{EnemyTeamAlive}</>" (3.2 0x2746410, redrawn when PlayersLeft changes) and
    // the map screen's team counts (0x2747150) show. AFortGameModeAthena's AddToAlivePlayers and RemoveFromAlivePlayers
    // (3.2 0x9AE8D0, 0x9CA730) count each player into it, on a playlist with bIsLargeTeamGame (Teams of 20) or 50v50's;
    // other playlists leave it empty.
    DefineUProperty(TArray<int32_t>, TeamXPlayersLeft)
    static bool HasTeamXPlayersLeft() { return Member_TeamXPlayersLeft.IsAvailable(); }
    // The warmup's countdown in the world's time, replicated for the clients' timer: set when the warmup starts
    // (UpdateWarmupPhase, 1.9 0x43D9A0, the end -1 until then), and the bus leaves at the end.
    DefineUProperty(float, WarmupCountdownStartTime)
    DefineUProperty(float, WarmupCountdownEndTime)

    // The map's AFortAthenaMapInfo, set by AFortGameModeAthena's own FinishWorldInitialization (1.7.2 0x3CF860).
    DefineUProperty(AFortAthenaMapInfo*, MapInfo)

    // 3.5: the match's playlist, set by SetCurrentPlaylistId (3.5.1 0x9FA020) from the game mode's playlist list.
    // ApplyPlaylistDataTables (0x9DC890) copies its tables over the default ones.
    DefineUProperty(UFortPlaylistAthena*, CurrentPlaylistData)
    static bool HasCurrentPlaylistData() { return Member_CurrentPlaylistData.IsAvailable(); }
    // Up to 5.21 the playlist's id is the game state's too (5.30 has none).
    DefineUProperty(int32_t, CurrentPlaylistId)
    static bool HasCurrentPlaylistId() { return Member_CurrentPlaylistId.IsAvailable(); }
    // Whether the playlist's assets have loaded (LoadCurrentPlaylistData, then OnPlaylistDataLoadCompleted), or are
    // loading.
    DefineUProperty(bool, bPlaylistDataIsLoaded)
    DefineUProperty(bool, bPlaylistDataIsActivelyLoading)
    // 5.30 0xBEC6C0: what a client does with a playlist that has replicated, and what the stripped server side did
    // after setting it (5.21's AFortGameModeAthena::OnGivenMatchAssignment, 0xBD59E0): the game state's values from it
    // (0xBD2710), then LoadCurrentPlaylistData (0xBD3AD0).
    DefineUFunction(OnRep_CurrentPlaylistData)
    // 3.5: the playlist's bIsLargeTeamGame, on the game state (the game mode's before 3.5, with bFiftyFiftySquadPlay,
    // which 3.5 dropped).
    DefineUProperty(bool, bIsLargeTeamGame)
    static bool HasLargeTeamGame() { return Member_bIsLargeTeamGame.IsAvailable(); }

    // 3.5: EAirCraftBehavior (1: OpposingAirCraftForEachTeam, 50v50's) and the aircrafts StartAircraftPhase spawns, one
    // per team in that behaviour. GetAircraftIndex (3.5.1 0x9E5750) maps a player state to its aircraft from them.
    DefineUProperty(uint8_t, AirCraftBehavior)
    DefineUProperty(TArray<AFortAthenaAircraft*>, Aircrafts)
    static bool HasAircrafts() { return Member_AirCraftBehavior.IsAvailable() && Member_Aircrafts.IsAvailable(); }

    static bool HasWarmupCountdown() { return Member_WarmupCountdownStartTime.IsAvailable() && Member_WarmupCountdownEndTime.IsAvailable(); }
};
