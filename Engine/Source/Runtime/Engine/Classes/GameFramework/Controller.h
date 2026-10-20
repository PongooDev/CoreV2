#pragma once

#include "GameFramework/Actor.h"

class APawn;
class APlayerState;

class AController : public AActor
{
    DefineUnrealClass(AController, Engine)
    DefineUFunction(Possess)

public:
    DefineUProperty(APawn*, Pawn)
    DefineUProperty(APlayerState*, PlayerState)

    APawn* GetPawn() const { return Pawn; }
    AActor* GetViewTarget() const;
};
