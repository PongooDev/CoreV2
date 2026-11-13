#include "pch.h"
#include "Components/CapsuleComponent.h"
#include "ReflectionParams.h"

float UCapsuleComponent::GetScaledCapsuleHalfHeight() const
{
    UFunction* Function = FindStaticFunction_GetScaledCapsuleHalfHeight();
    Reflection::FParams Params(Function);
    return Function && Params.Call(const_cast<UCapsuleComponent*>(this)) ? Params.Take<float>(L"ReturnValue") : 0.0f;
}
