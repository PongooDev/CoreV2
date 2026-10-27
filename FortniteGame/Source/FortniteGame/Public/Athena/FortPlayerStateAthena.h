#pragma once

#include "FortTeamInfo.h"
#include "Player/FortPlayerState.h"

// Battle Royale's player state (from 1.7.2).
class AFortPlayerStateAthena : public AFortPlayerStateZone
{
    DefineUnrealClass(AFortPlayerStateAthena, FortniteGame)

public:
    // The player's team, copied from its AFortTeamInfo when it joins one (0x44AB80).
    DefineUProperty(EFortTeam, TeamIndex)
    // Set when the player's team is out of the match (the teams left, for each player of it: 0x44A870) or has won (1).
    DefineUProperty(int32_t, Place)
    // Other teams' players the player eliminated: Kills up to 1.7.2, KillScore from 1.8 (GetKills).
    DefineUProperty(int32_t, Kills)
    DefineUProperty(int32_t, KillScore)
    // Set when the player is eliminated or wins.
    DefineUProperty(int32_t, SecondsAlive)
    // Client only, not replicated: whether the player is talking in voice chat, which the HUD's team list shows (its
    // speaker icon). Only Vivox's participant updates set it (1.7.2 0x3C54D20); Core sets it from the engine's voice chat.
    DefineUProperty(bool, bIsTalking)

    int32_t GetKills()
    {
        return Member_Kills.IsAvailable() ? Kills : Member_KillScore.IsAvailable() ? KillScore : 0;
    }
};
