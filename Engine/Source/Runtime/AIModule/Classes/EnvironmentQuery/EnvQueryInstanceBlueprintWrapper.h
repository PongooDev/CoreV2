#pragma once

#include "Containers/Array.h"
#include "UObject/Object.h"

class AActor;

class UEnvQueryInstanceBlueprintWrapper : public UObject
{
    DefineUnrealClass(UEnvQueryInstanceBlueprintWrapper, AIModule)
    DefineUFunction(GetResultsAsActors)

public:
    TArray<AActor*> GetResultsAsActors();
};
