#pragma once

#include "UObject/Object.h"

class APlayerController;

class UPlayer : public UObject
{
    DefineUnrealClass(UPlayer, Engine)

public:
    DefineUProperty(APlayerController*, PlayerController)
};
