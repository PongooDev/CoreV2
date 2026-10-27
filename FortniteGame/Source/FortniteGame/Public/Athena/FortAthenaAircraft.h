#pragma once

#include "GameFramework/Actor.h"

// Battle Royale's battle bus (from 1.7.2; AthenaAircraft_C). While it flies it is the start spot of every player in it
// (AFortGameModeAthena::ChoosePlayerStart, 0x3CA260), when they jump and at the forced drop at the end of its path.
class AFortAthenaAircraft : public AActor
{
    DefineUnrealClass(AFortAthenaAircraft, FortniteGame)
};
