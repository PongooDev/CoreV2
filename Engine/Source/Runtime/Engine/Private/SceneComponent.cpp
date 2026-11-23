#include "pch.h"
#include "Components/SceneComponent.h"
#include "ReflectionParams.h"

FVector USceneComponent::GetComponentLocation() const
{
    UFunction* Function = FindStaticFunction_K2_GetComponentLocation();
    Reflection::FParams Params(Function);
    return Function && Params.Call(const_cast<USceneComponent*>(this)) ? Params.Take<FVector>(L"ReturnValue") : FVector(0.0f, 0.0f, 0.0f);
}
