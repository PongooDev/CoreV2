#pragma once

#include "UObject/Object.h"

class AFortObjectiveBase;

class UFortObjectiveFailedParams : public UObject
{
    DefineUnrealClass(UFortObjectiveFailedParams, FortniteGame)
    DefineUFunction(SetParams)

public:
    UFortObjectiveFailedParams* SetParams(AFortObjectiveBase* FailedObjective);
};
