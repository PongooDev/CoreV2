#pragma once

#include "Athena/Mutators/FortAthenaMutator.h"

class AController;
class UWorld;

// 9.01's Wick's Bounty (Playlist_Wax_Solo, _Duos, _Squads; Mutator_Wax_C). Each player's tokens and lives are an entry
// of the mutator's PlayerData (FWaxPlayerDataEntry: CurrentTokens, CurrentLives, TokenBasedPlacement). 9.01 (CL-6428087)
// keeps all of it native:
// - the entry: its answer to AFortGameModeZone::GetStartingItems (FortGameModeAthena_GetInventoryOverride, slot 0xFB,
//   0x11095A0) adds a player who has none, with LivesToStartPlayerWith lives, and sets their tokens to at least
//   TokensToStartWith (SetTokens, 0x111D460). Nothing left in the build calls GetStartingItems (AFortGameModeAthena::
//   GiveMutatorStartingItems is Core's call of it); a player with no entry has no lives and no tokens.
// - the payout: FortGameStateAthena_OnClientPawnLoaded (slot 0x116, 0x110ABF0), which the game state asks of its mutators
//   (slot 0x123, 0x105E520) from AFortPlayerController::ServerClientPawnLoaded_Implementation (0x17BC580), binds
//   CommonDeadPawn (0x10FED40) to the new pawn's OnFortPlayerDied. It takes a life, and gives the dead player's tokens to
//   the player state in their DeathInfo.Downer (another team's, the death stored: bInitialized), else drops them as an
//   AFortGameModePickup_Wax where they died.
// - SetTokens: the player state's Score, the visibility tier's gameplay effects (VisibilityModifiers, 0x111A300) and
//   the three leaders above the threshold (First/Second/ThirdPlaceIfAboveThreshold). The game mode's Tick (0x10361C0)
//   checks the team scores against GetScoringModeWinningGoal (slot 0xFD: TokensToWin; MutatorControlledGoalScore,
//   0x100D1C0) and then asks the mutators' PostTick (slot 0xCD, 0x11062E0), whose team sum (0x1124A50) and leaders
//   (TeamLeadersInOrder, 0x111A5E0) replicate.
// - the lives: the game state's IsRespawningAllowed (slot 0x125) asks slot 0xE5 (0x110A960, CurrentLives > 0), and the
//   respawn invalidation timer (0x1095C90) takes out a player whose last life CommonDeadPawn took.
class AFortAthenaMutator_Wax : public AFortAthenaMutator
{
    DefineUnrealClass(AFortAthenaMutator_Wax, FortniteGame)

public:
    static AFortAthenaMutator_Wax* GetCurrent(UWorld* World);
};
