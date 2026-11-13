#pragma once

#include "Components/PrimitiveComponent.h"
#include "Reflection.h"

class UCapsuleComponent : public UPrimitiveComponent
{
    DefineUnrealClass(UCapsuleComponent, Engine)
    DefineUFunction(GetScaledCapsuleHalfHeight)

public:
    // The capsule's half height with the component's scale (UCapsuleComponent::GetScaledCapsuleHalfHeight), 0 where the
    // build has no such function.
    float GetScaledCapsuleHalfHeight() const;
};
