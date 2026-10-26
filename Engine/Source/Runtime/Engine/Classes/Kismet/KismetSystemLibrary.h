#pragma once

#include "Math/Vector.h"
#include "UObject/Object.h"

class USceneComponent;

class UKismetSystemLibrary : public UObject
{
    DefineUnrealClass(UKismetSystemLibrary, Engine)
    DefineUFunction(GetComponentBounds)

public:
    static void GetComponentBounds(const USceneComponent* Component, FVector& Origin, FVector& BoxExtent, float& SphereRadius);
};
