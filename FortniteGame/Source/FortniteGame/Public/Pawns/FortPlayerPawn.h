#pragma once

#include "Pawns/FortPawn.h"

class AFortPlayerPawn : public AFortPawn
{
    DefineUnrealClass(AFortPlayerPawn, FortniteGame)

public:
    void ResetGameplayAttributes();
};
