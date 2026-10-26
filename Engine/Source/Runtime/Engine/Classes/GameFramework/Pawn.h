#pragma once

#include "GameFramework/Actor.h"

class APlayerState;

class APawn : public AActor
{
    DefineUnrealClass(APawn, Engine)

public:
    DefineUProperty(AController*, Controller)
    DefineUProperty(APlayerState*, PlayerState)
};
