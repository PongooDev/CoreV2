#pragma once

#include "UObject/Class.h"
#include "UObject/NameTypes.h"

namespace EEnvQueryRunMode
{
    enum Type
    {
        SingleResult,
        RandomBest5Pct,
        RandomBest25Pct,
        AllMatching,
    };
}

// How a query ended, as its wrapper's OnQueryFinishedEvent passes it (FEQSQueryDoneSignature's QueryStatus).
namespace EEnvQueryStatus
{
    enum Type
    {
        Processing,
        Success,
        Failed,
        Aborted,
        OwnerLost,
        MissingParam,
    };
}

class EAIParamType
{
    DefineUnrealEnum(EAIParamType, AIModule)
};

// A named parameter of an environment query (the query's own params, overridden by this value).
struct FEnvNamedValue
{
    DefineUnrealStruct(FEnvNamedValue, AIModule)

public:
    DefineUProperty(FName, ParamName)
    DefineUProperty(EAIParamType, ParamType)
    DefineUProperty(float, Value)
};

// A query parameter as an EQS caller configures it (FEQSParametrizedQueryExecutionRequest's QueryConfig): the named
// param and its value, or a blackboard key to read it from.
struct FAIDynamicParam
{
    DefineUnrealStruct(FAIDynamicParam, AIModule)

public:
    DefineUProperty(FName, ParamName)
    DefineUProperty(EAIParamType, ParamType)
    DefineUProperty(float, Value)
};
