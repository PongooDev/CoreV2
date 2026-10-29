#pragma once

#include "GameFramework/Actor.h"

class APlayerState;

class APawn : public AActor
{
    DefineUnrealClass(APawn, Engine)

public:
    DefineUProperty(AController*, Controller)
    DefineUProperty(APlayerState*, PlayerState)
    // EAutoPossessAI: whether an AI controller takes the pawn when it is spawned (0 never, 2 or 3 when spawned).
    DefineUProperty(uint8_t, AutoPossessAI)
};
