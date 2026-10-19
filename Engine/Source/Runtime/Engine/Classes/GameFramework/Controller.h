#pragma once

#include "GameFramework/Actor.h"

class APawn;

class AController : public AActor
{
    DefineUnrealClass(AController, Engine)

public:
    DefineUProperty(APawn*, Pawn)

    APawn* GetPawn() const { return Pawn; }
    AActor* GetViewTarget() const;
};
