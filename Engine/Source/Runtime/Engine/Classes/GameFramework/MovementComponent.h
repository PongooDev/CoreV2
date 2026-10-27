#pragma once

#include "Components/ActorComponent.h"
#include "Math/Vector.h"
#include "Reflection.h"

class USceneComponent;

class UMovementComponent : public UActorComponent
{
    DefineUnrealClass(UMovementComponent, Engine)
    DefineUFunction(GetGravityZ)

public:
    DefineUProperty(USceneComponent*, UpdatedComponent)
    DefineUProperty(FVector, Velocity)

    // The gravity the component moves under (a projectile's is scaled by its ProjectileGravityScale).
    float GetGravityZ() const;
    // Whether the component has stopped moving anything (a projectile that came to rest: StopSimulating clears its
    // UpdatedComponent). False when the build doesn't reflect UpdatedComponent.
    bool HasStoppedSimulating() const;
};
