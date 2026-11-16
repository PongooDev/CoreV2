#pragma once

#include "GameFramework/Info.h"
#include "Reflection.h"

class AFortGameStateAthena;
class AFortPlayerController;
class AFortPlayerStateAthena;

// Battle Royale's gameplay mutator (from 6.01): an AFortGameplayMutator in the game. Core declares no
// AFortGameplayMutator, so this derives from its AInfo. The playlist's modifiers spawn its subclasses. Those that change
// the game state's answers (bMutatesGameState) are in AFortGameStateAthena's GameplayMutators, and the game state's code
// asks each of them through its FortGameStateAthena_* virtuals.
class AFortAthenaMutator : public AInfo
{
    DefineUnrealClass(AFortAthenaMutator, FortniteGame)

public:
    // AFortGameplayMutator's (IsMutatorActive, 6.31 0x10FC7C0, reads it): the game state's loops over its mutators skip
    // one that isn't active (OnPlaylistTimerTicked's, 0xCAF640).
    DefineBitfieldUProperty(bMutatorActive)
    static bool HasMutatorActive() { return Member_bMutatorActive.IsAvailable(); }

    // 21.40's FortGameStateAthena_OnPostEarnedKill: a player earned a kill. 21.40 passes their player state; 6.31
    // passes their controller (Finder::ResolveOnPostEarnedKillLive checks that the override reads it as one).
    // AFortAthenaMutator's does nothing. 6.31's Team Rumble (AFortAthenaMutator_TDM, 0xD3C1B0) adds the kill to the
    // killer team's AFortTeamInfoAthena CurrentTeamScore and to the game state's MutatorGenericInt_0 or _1, the scores
    // its HUD shows. Nothing in the client build calls it: the server's call was stripped with the kill handling.
    // Server only; nothing on a build where the slot isn't found.
    static bool HasOnPostEarnedKill();
    void FortGameStateAthena_OnPostEarnedKill(AFortPlayerController* KillerController);
};

// Team Rumble's mutator (6.31's Playlist_Respawn_24, RespawnMutator_C). Its FortGameStateAthena_OnPostEarnedKill scores
// the teams. Its GetScoringModeWinningGoal (0xD3A0C0) gives the game mode's Tick the eliminations that win
// (ActualNumKillsForVictory, set at its BeginPlay from NumKillsForVictory).
class AFortAthenaMutator_TDM : public AFortAthenaMutator
{
    DefineUnrealClass(AFortAthenaMutator_TDM, FortniteGame)

public:
    // Whether it keeps the two teams' scores in the game state's MutatorGenericInt_0 and _1.
    DefineUProperty(bool, bIsTwoTeamTDM)

    // A kill by Core's player bot, which the override can't take. The override reads the earner's team from its
    // controller (AFortPlayerController's team member interface), and a bot's controller is a stock AAIController,
    // which has none. Epic's 6.31 has no AI players to ask for. So this does what 0xD3C1B0 does, with the bot's team
    // and its AFortTeamInfoAthena (the player state's PlayerTeam): one more on its CurrentTeamScore, copied with
    // bIsTwoTeamTDM to MutatorGenericInt_0 for HumanPvP_Team1 or _1 for the other team. Core's.
    void ScoreBotKill(AFortGameStateAthena* GameState, AFortPlayerStateAthena* KillerPlayerState);
};
