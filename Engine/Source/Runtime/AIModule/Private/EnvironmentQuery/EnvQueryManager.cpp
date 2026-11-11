#include "pch.h"
#include "EnvironmentQuery/EnvQueryManager.h"
#include "ReflectionParams.h"
#include "Templates/Casts.h"

UEnvQueryInstanceBlueprintWrapper* UEnvQueryManager::RunEQSQuery(UObject* WorldContext, UEnvQuery* QueryTemplate, UObject* Querier, EEnvQueryRunMode::Type RunMode, UClass* WrapperClass)
{
    Reflection::FParams Params(StaticFunction_RunEQSQuery());
    Params.Set(Params.Has(L"WorldContextObject") ? L"WorldContextObject" : L"WorldContext", WorldContext);
    Params.Set(L"QueryTemplate", QueryTemplate);
    Params.Set(L"Querier", Querier);
    Params.Set(L"RunMode", static_cast<uint8_t>(RunMode));
    Params.Set(L"WrapperClass", WrapperClass);
    return Params.Call(StaticClass()->GetDefaultObject()) ? Params.Take<UEnvQueryInstanceBlueprintWrapper*>(L"ReturnValue") : nullptr;
}

std::optional<bool> UEnvQueryManager::IsQueryRunning(UEnvQueryInstanceBlueprintWrapper* QueryInstance)
{
    UObject* Outer = QueryInstance && Member_GCShieldedWrappers.IsAvailable() ? reinterpret_cast<UObject*>(QueryInstance)->GetOuter() : nullptr;
    UEnvQueryManager* Manager = Cast<UEnvQueryManager>(Outer);
    if (!Manager)
    {
        return std::nullopt;
    }
    const TArray<UEnvQueryInstanceBlueprintWrapper*>& Wrappers = Manager->GCShieldedWrappers;
    for (int32_t Index = 0; Index < Wrappers.Num(); ++Index)
    {
        if (Wrappers[Index] == QueryInstance)
        {
            return true;
        }
    }
    return false;
}
