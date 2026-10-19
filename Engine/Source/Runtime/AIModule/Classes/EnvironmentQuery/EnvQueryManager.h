#pragma once

#include "EnvironmentQuery/EnvQueryTypes.h"
#include "UObject/Object.h"

class UEnvQuery;
class UEnvQueryInstanceBlueprintWrapper;

class UEnvQueryManager : public UObject
{
    DefineUnrealClass(UEnvQueryManager, AIModule)
    DefineUFunction(RunEQSQuery)

public:
    static UEnvQueryInstanceBlueprintWrapper* RunEQSQuery(UObject* WorldContext, UEnvQuery* QueryTemplate, UObject* Querier, EEnvQueryRunMode::Type RunMode, UClass* WrapperClass);
};
