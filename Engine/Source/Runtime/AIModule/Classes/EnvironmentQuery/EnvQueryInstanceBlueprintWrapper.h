#pragma once

#include "Containers/Array.h"
#include "Math/Vector.h"
#include "UObject/Object.h"

class AActor;

class UEnvQueryInstanceBlueprintWrapper : public UObject
{
    DefineUnrealClass(UEnvQueryInstanceBlueprintWrapper, AIModule)
    DefineUFunction(GetResultsAsActors)
    DefineUFunction(GetResultsAsLocations)
    DefineUFunction(SetNamedParam)

public:
    TArray<AActor*> GetResultsAsActors();
    // The items' locations, best score first (a point generator's items, or each actor's location); empty until the
    // query is done.
    TArray<FVector> GetResultsAsLocations();
    // Adds a named param to the running query instance (its NamedParams, which the generators' and tests' data bindings
    // read when the instance runs on the EQS manager's tick). False where the build has no SetNamedParam.
    bool SetNamedParam(FName ParamName, float Value);
};
