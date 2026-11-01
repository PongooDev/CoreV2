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
