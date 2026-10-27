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
};
