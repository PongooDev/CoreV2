#pragma once

#include "Math/Vector.h"
#include "UObject/Object.h"

class AActor;

class UAISense_Hearing : public UObject
{
    DefineUnrealClass(UAISense_Hearing, AIModule)
    DefineUFunction(ReportNoiseEvent)

public:
    static void ReportNoiseEvent(UObject* WorldContextObject, const FVector& NoiseLocation, float Loudness, AActor* Instigator, float MaxRange, FName Tag);
};
