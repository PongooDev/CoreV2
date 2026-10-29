#pragma once

#include "GameFramework/Actor.h"

class APawn;
class APlayerState;

class AController : public AActor
{
    DefineUnrealClass(AController, Engine)
    DefineUFunction(Possess)
    DefineUFunction(UnPossess)

public:
    DefineUProperty(APawn*, Pawn)
    DefineUProperty(APlayerState*, PlayerState)

    APawn* GetPawn() const { return Pawn; }
    AActor* GetViewTarget() const;
    // Server: lets go of the pawn (AController::UnPossess, through its UFunction).
    void UnPossess();
};
