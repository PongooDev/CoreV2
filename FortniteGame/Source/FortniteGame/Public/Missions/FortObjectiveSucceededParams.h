#pragma once

#include "UObject/Object.h"

class AFortObjectiveBase;

class UFortObjectiveSucceededParams : public UObject
{
    DefineUnrealClass(UFortObjectiveSucceededParams, FortniteGame)
    DefineUFunction(SetParams)

public:
    UFortObjectiveSucceededParams* SetParams(AFortObjectiveBase* SucceededObjective);
};
