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

public:
    TArray<AActor*> GetResultsAsActors();
    // The items' locations, best score first (a point generator's items, or each actor's location); empty until the
    // query is done.
    TArray<FVector> GetResultsAsLocations();
};
