#include "pch.h"
#include "Building/BuildingActor.h"
#include "ReflectionParams.h"

float ABuildingActor::GetHealth() const
{
    Reflection::FParams Params(StaticFunction_GetHealth());
    return Params.Call(const_cast<ABuildingActor*>(this)) ? Params.Take<float>(L"ReturnValue") : 0.0f;
}

FVector ABuildingActor::GetCentroid() const
{
    Reflection::FParams Params(StaticFunction_GetCentroid());
    return Params.Call(const_cast<ABuildingActor*>(this)) ? Params.Take<FVector>(L"ReturnValue") : FVector();
}

float ABuildingActor::GetMaxHealth() const
{
    Reflection::FParams Params(StaticFunction_GetMaxHealth());
    return Params.Call(const_cast<ABuildingActor*>(this)) ? Params.Take<float>(L"ReturnValue") : 0.0f;
}
