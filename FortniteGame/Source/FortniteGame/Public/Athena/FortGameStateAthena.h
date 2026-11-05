#pragma once

#include "FortGameState.h"

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
    // (1.7.2 0x437C30) only takes a player out of the alive players from Aircraft on.
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

    static bool HasWarmupCountdown() { return Member_WarmupCountdownStartTime.IsAvailable() && Member_WarmupCountdownEndTime.IsAvailable(); }
};
