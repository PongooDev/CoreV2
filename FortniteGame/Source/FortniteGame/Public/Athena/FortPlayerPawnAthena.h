#pragma once

#include "Athena/FortPlayerControllerAthena.h"
#include "Pawns/FortPlayerPawn.h"

// Battle Royale's player pawn (from 1.7.2; PlayerPawn_Athena_C).
class AFortPlayerPawnAthena : public AFortPlayerPawn
{
    DefineUnrealClass(AFortPlayerPawnAthena, FortniteGame)

public:
    // From leaving the aircraft (0x415D90) until landing (0x41AB80).
    DefineUProperty(bool, bIsSkydiving)
    // 1.8: the controller's loadout, copied by its Possess (0x444480). Every client sets up the glider of every pawn's
    // (OnRep_CustomizationLoadout, 0x43D4A0), and an empty one crashes them all.
    DefineUStructProperty(FFortAthenaLoadout, CustomizationLoadout)
    // How many teammates are reviving the pawn (AFortPlayerControllerAthena::ServerDBNOReviveStarted and Interrupted).
    DefineUProperty(uint8_t, DBNORevivalStacking)

    bool HasCustomizationLoadout() { return Member_CustomizationLoadout.IsAvailable(); }
};
