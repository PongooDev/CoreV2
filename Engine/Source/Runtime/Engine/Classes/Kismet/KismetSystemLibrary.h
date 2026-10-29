#pragma once

#include "Math/Vector.h"
#include "UObject/Object.h"

class USceneComponent;

class UKismetSystemLibrary : public UObject
{
    DefineUnrealClass(UKismetSystemLibrary, Engine)
    DefineUFunction(GetComponentBounds)
    DefineUFunction(LineTraceSingle)
    DefineUFunction(K2_SetTimer)
    DefineUFunction(K2_ClearTimer)

public:
    static void GetComponentBounds(const USceneComponent* Component, FVector& Origin, FVector& BoxExtent, float& SphereRadius);
    // The first blocking hit's impact point from Start to End on a trace channel (ETraceTypeQuery: 0 is Visibility), no
    // actors ignored but the world context object when bIgnoreSelf. False when nothing blocks.
    static bool LineTraceSingle(UObject* WorldContextObject, const FVector& Start, const FVector& End, uint8_t TraceChannel, bool bTraceComplex, bool bIgnoreSelf, FVector& OutImpactPoint);
    // Runs Object's UFunction FunctionName (no parameters) every Time seconds when bLooping, else once after Time, on
    // Object's world's timer manager. K2_ClearTimer clears the timers set for it so.
    static bool K2_SetTimer(UObject* Object, const wchar_t* FunctionName, float Time, bool bLooping);
    static void K2_ClearTimer(UObject* Object, const wchar_t* FunctionName);
};
