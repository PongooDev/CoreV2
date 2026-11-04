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
    DefineUProperty(FRotator, ControlRotation)

    APawn* GetPawn() const { return Pawn; }
    AActor* GetViewTarget() const;
    // Server: lets go of the pawn (AController::UnPossess, through its UFunction).
    void UnPossess();
    // Server: takes InPawn (AController::Possess, through its UFunction).
    void Possess(APawn* InPawn);
};
