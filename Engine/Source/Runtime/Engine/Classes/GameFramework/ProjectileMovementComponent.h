#pragma once

#include "GameFramework/MovementComponent.h"
#include "Reflection.h"
#include "UObject/ScriptDelegates.h"

class UProjectileMovementComponent : public UMovementComponent
{
    DefineUnrealClass(UProjectileMovementComponent, Engine)

public:
    DefineUProperty(float, MaxSpeed)
    DefineUProperty(float, Bounciness)
    DefineUProperty(float, Friction)
    // Broadcast by HandleImpact on each bounce (FOnProjectileBounceDelegate: the hit and the velocity before it), before
    // the bounce's velocity is checked against the stop threshold.
    DefineUProperty(FMulticastScriptDelegate, OnProjectileBounce)
    // Broadcast by StopSimulating (FOnProjectileStopDelegate: the hit it stopped on), after it zeroes the velocity and
    // clears UpdatedComponent.
    DefineUProperty(FMulticastScriptDelegate, OnProjectileStop)
};
