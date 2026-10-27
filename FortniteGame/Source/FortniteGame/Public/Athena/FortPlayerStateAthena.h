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
    // Other teams' players the player eliminated.
    DefineUProperty(int32_t, Kills)
    // Set when the player is eliminated or wins.
    DefineUProperty(int32_t, SecondsAlive)
};
