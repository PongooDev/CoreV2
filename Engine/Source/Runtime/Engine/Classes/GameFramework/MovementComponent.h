#pragma once

#include "Components/ActorComponent.h"
#include "Math/Vector.h"
#include "Reflection.h"

class UMovementComponent : public UActorComponent
{
    DefineUnrealClass(UMovementComponent, Engine)
    DefineUFunction(GetGravityZ)

public:
    DefineUProperty(FVector, Velocity)

    // The gravity the component moves under (a projectile's is scaled by its ProjectileGravityScale).
    float GetGravityZ() const;
};
