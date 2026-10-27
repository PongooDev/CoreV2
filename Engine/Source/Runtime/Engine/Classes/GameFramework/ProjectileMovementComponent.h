#pragma once

#include "GameFramework/MovementComponent.h"
#include "Reflection.h"

class UProjectileMovementComponent : public UMovementComponent
{
    DefineUnrealClass(UProjectileMovementComponent, Engine)

public:
    DefineUProperty(float, MaxSpeed)
};
