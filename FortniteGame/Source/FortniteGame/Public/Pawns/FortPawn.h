#pragma once

#include "GameFramework/Pawn.h"

class AFortPawn : public APawn
{
    DefineUnrealClass(AFortPawn, FortniteGame)
    DefineUFunction(IsDead)

public:
    bool IsDead() const;
};
