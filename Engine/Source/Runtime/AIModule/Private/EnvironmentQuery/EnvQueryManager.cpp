#include "pch.h"
#include "EnvironmentQuery/EnvQueryManager.h"
#include "ReflectionParams.h"

UEnvQueryInstanceBlueprintWrapper* UEnvQueryManager::RunEQSQuery(UObject* WorldContext, UEnvQuery* QueryTemplate, UObject* Querier, EEnvQueryRunMode::Type RunMode, UClass* WrapperClass)
{
    Reflection::FParams Params(StaticFunction_RunEQSQuery());
    Params.Set(L"WorldContext", WorldContext);
    Params.Set(L"QueryTemplate", QueryTemplate);
    Params.Set(L"Querier", Querier);
    Params.Set(L"RunMode", static_cast<uint8_t>(RunMode));
    Params.Set(L"WrapperClass", WrapperClass);
    return Params.Call(StaticClass()->GetDefaultObject()) ? Params.Take<UEnvQueryInstanceBlueprintWrapper*>(L"ReturnValue") : nullptr;
}
