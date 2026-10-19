#include "pch.h"
#include "EnvironmentQuery/EnvQueryInstanceBlueprintWrapper.h"
#include "ReflectionParams.h"

TArray<AActor*> UEnvQueryInstanceBlueprintWrapper::GetResultsAsActors()
{
    Reflection::FParams Params(StaticFunction_GetResultsAsActors());
    return Params.Call(this) ? Params.Take<TArray<AActor*>>(L"ReturnValue") : TArray<AActor*>();
}
