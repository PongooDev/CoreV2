#pragma once

#include "Athena/FortAthenaVehicle.h"
#include "Reflection.h"

// The Pirate Cannon (8.00; AFortAthenaSKPushVehicle). Seat 0 pushes and aims, seat 1 is the passenger the cannon fires.
// Its Blueprint launches the passenger from OnLaunchPawn, after OnPreLaunchPawn; MultiCastPushCannonLaunchedPlayer
// plays the shot on everyone's game.
class AFortAthenaSKPushCannon : public AFortAthenaVehicle
{
    DefineUnrealClass(AFortAthenaSKPushCannon, FortniteGame)

public:
    DefineUFunction(OnPreLaunchPawn)
    DefineUFunction(OnLaunchPawn)
    DefineUFunction(MultiCastPushCannonLaunchedPlayer)
};
