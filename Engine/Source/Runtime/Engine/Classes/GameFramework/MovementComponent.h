#pragma once

#include "Components/ActorComponent.h"
#include "Math/Vector.h"
#include "Reflection.h"

class UMovementComponent : public UActorComponent
{
    DefineUnrealClass(UMovementComponent, Engine)

public:
    DefineUProperty(FVector, Velocity)
};
