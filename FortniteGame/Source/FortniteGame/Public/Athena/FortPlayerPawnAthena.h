#pragma once

#include "Pawns/FortPlayerPawn.h"

// Battle Royale's player pawn (from 1.7.2; PlayerPawn_Athena_C).
class AFortPlayerPawnAthena : public AFortPlayerPawn
{
    DefineUnrealClass(AFortPlayerPawnAthena, FortniteGame)

public:
    // From leaving the aircraft (0x415D90) until landing (0x41AB80).
    DefineUProperty(bool, bIsSkydiving)
};
