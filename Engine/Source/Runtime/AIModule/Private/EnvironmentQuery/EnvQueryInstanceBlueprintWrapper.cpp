#include "pch.h"
#include "EnvironmentQuery/EnvQueryInstanceBlueprintWrapper.h"
#include "ReflectionParams.h"

TArray<AActor*> UEnvQueryInstanceBlueprintWrapper::GetResultsAsActors()
{
    Reflection::FParams Params(StaticFunction_GetResultsAsActors());
    return Params.Call(this) ? Params.Take<TArray<AActor*>>(L"ReturnValue") : TArray<AActor*>();
}

TArray<FVector> UEnvQueryInstanceBlueprintWrapper::GetResultsAsLocations()
{
    Reflection::FParams Params(FindStaticFunction_GetResultsAsLocations());
    return Params.Call(this) ? Params.Take<TArray<FVector>>(L"ReturnValue") : TArray<FVector>();
}

bool UEnvQueryInstanceBlueprintWrapper::SetNamedParam(FName ParamName, float Value)
{
    UFunction* Function = FindStaticFunction_SetNamedParam();
    if (!Function)
    {
        return false;
    }
    Reflection::FParams Params(Function);
    Params.Set(L"ParamName", ParamName);
    Params.Set(L"Value", Value);
    return Params.Call(this);
}
