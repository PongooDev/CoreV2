#include "pch.h"
#include "GameFramework/MovementComponent.h"
#include "ReflectionParams.h"

float UMovementComponent::GetGravityZ() const
{
    Reflection::FParams Params(StaticFunction_GetGravityZ());
    return Params.Call(const_cast<UMovementComponent*>(this)) ? Params.Take<float>(L"ReturnValue") : 0.0f;
}

bool UMovementComponent::HasStoppedSimulating() const
{
    return Member_UpdatedComponent.IsAvailable() && !static_cast<USceneComponent*>(UpdatedComponent);
}
