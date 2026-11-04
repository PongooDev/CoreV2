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
    DefineUFunction(AddMovementInput)
    DefineUFunction(OnRep_PlayerState)

    // Adds to the movement the pawn's movement component consumes on its next tick (an AI controller's pawn on the
    // server too).
    void AddMovementInput(const FVector& WorldDirection, float ScaleValue = 1.0f, bool bForce = false);
    // The pawn's reaction to its player state (a Fortnite player pawn takes its ability system from it there).
    void OnRep_PlayerState();
};
