#include "pch.h"
#include "Perception/AISense_Hearing.h"
#include "ReflectionParams.h"

void UAISense_Hearing::ReportNoiseEvent(UObject* WorldContextObject, const FVector& NoiseLocation, float Loudness, AActor* Instigator, float MaxRange, FName Tag)
{
    Reflection::FParams Params(StaticFunction_ReportNoiseEvent());
    Params.Set(Params.Has(L"WorldContextObject") ? L"WorldContextObject" : L"WorldContext", WorldContextObject);
    Params.Set(L"NoiseLocation", NoiseLocation);
    Params.Set(L"Loudness", Loudness);
    Params.Set(L"Instigator", Instigator);
    Params.Set(L"MaxRange", MaxRange);
    Params.Set(L"Tag", Tag);
    Params.Call(StaticClass()->GetDefaultObject());
}
