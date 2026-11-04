#pragma once

#include "GameFramework/Actor.h"

// Battle Royale's battle bus (from 1.7.2; AthenaAircraft_C). While it flies it is the start spot of every player in it
// (AFortGameModeAthena::ChoosePlayerStart, 0x3CA260), when they jump and at the forced drop at the end of its path.
class AFortAthenaAircraft : public AActor
{
    DefineUnrealClass(AFortAthenaAircraft, FortniteGame)

public:
    // When its players may jump (world seconds; replicated): the drop zone, from entering to leaving it.
    DefineUProperty(float, DropStartTime)
    DefineUProperty(float, DropEndTime)
    static bool HasDropTimes() { return Member_DropStartTime.IsAvailable() && Member_DropEndTime.IsAvailable(); }
};
