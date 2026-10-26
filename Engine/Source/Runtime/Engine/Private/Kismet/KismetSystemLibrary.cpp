#include "pch.h"
#include "Kismet/KismetSystemLibrary.h"
#include "ReflectionParams.h"

void UKismetSystemLibrary::GetComponentBounds(const USceneComponent* Component, FVector& Origin, FVector& BoxExtent, float& SphereRadius)
{
    Reflection::FParams Params(StaticFunction_GetComponentBounds());
    Params.Set(L"Component", Component);
    Origin = FVector();
    BoxExtent = FVector();
    SphereRadius = 0.0f;
    if (Component && Params.Call(StaticClass()->GetDefaultObject()))
    {
        Origin = Params.Take<FVector>(L"Origin");
        BoxExtent = Params.Take<FVector>(L"BoxExtent");
        SphereRadius = Params.Take<float>(L"SphereRadius");
    }
}
