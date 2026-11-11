#pragma once

#include "Containers/Array.h"
#include "EnvironmentQuery/EnvQueryTypes.h"
#include "UObject/Object.h"

#include <optional>

class UEnvQuery;
class UEnvQueryInstanceBlueprintWrapper;

class UEnvQueryManager : public UObject
{
    DefineUnrealClass(UEnvQueryManager, AIModule)
    DefineUFunction(RunEQSQuery)
    DefineUProperty(TArray<UEnvQueryInstanceBlueprintWrapper*>, GCShieldedWrappers)

public:
    static UEnvQueryInstanceBlueprintWrapper* RunEQSQuery(UObject* WorldContext, UEnvQuery* QueryTemplate, UObject* Querier, EEnvQueryRunMode::Type RunMode, UClass* WrapperClass);

    // Whether QueryInstance's query still runs: RunEQSQuery keeps the wrapper in its manager's GCShieldedWrappers while
    // the query runs (RegisterActiveWrapper), and the wrapper's OnQueryFinished takes it out (UnregisterActiveWrapper)
    // once its result is in. Unset where the build has no GCShieldedWrappers.
    static std::optional<bool> IsQueryRunning(UEnvQueryInstanceBlueprintWrapper* QueryInstance);
};
