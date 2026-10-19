#pragma once

#include "GameFramework/Actor.h"

class APawn;

class AController : public AActor
{
    DefineUnrealClass(AController, Engine)

public:
    DefineUProperty(APawn*, Pawn)

    AActor* GetViewTarget() const;
};
