#pragma once

#include "Math/Vector.h"
#include "UObject/Object.h"

class USceneComponent;

class UKismetSystemLibrary : public UObject
{
    DefineUnrealClass(UKismetSystemLibrary, Engine)
    DefineUFunction(GetComponentBounds)
    DefineUFunction(LineTraceSingle)

public:
    static void GetComponentBounds(const USceneComponent* Component, FVector& Origin, FVector& BoxExtent, float& SphereRadius);
    // The first blocking hit's impact point from Start to End on a trace channel (ETraceTypeQuery: 0 is Visibility), no
    // actors ignored but the world context object when bIgnoreSelf. False when nothing blocks.
    static bool LineTraceSingle(UObject* WorldContextObject, const FVector& Start, const FVector& End, uint8_t TraceChannel, bool bTraceComplex, bool bIgnoreSelf, FVector& OutImpactPoint);
};
