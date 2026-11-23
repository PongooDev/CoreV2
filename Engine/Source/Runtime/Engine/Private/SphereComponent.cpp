#include "pch.h"
#include "Components/SphereComponent.h"
#include "ReflectionParams.h"

void USphereComponent::SetSphereRadius(float InSphereRadius, bool bUpdateOverlaps)
{
    UFunction* Function = FindStaticFunction_SetSphereRadius();
    Reflection::FParams Params(Function);
    if (Function && Params.Set(L"InSphereRadius", InSphereRadius) && Params.Set(L"bUpdateOverlaps", bUpdateOverlaps))
    {
        Params.Call(this);
    }
}
