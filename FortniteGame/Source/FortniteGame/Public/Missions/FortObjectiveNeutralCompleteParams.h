#pragma once

#include "UObject/Object.h"

class AFortObjectiveBase;

class UFortObjectiveNeutralCompleteParams : public UObject
{
    DefineUnrealClass(UFortObjectiveNeutralCompleteParams, FortniteGame)
    DefineUFunction(SetParams)

public:
    UFortObjectiveNeutralCompleteParams* SetParams(AFortObjectiveBase* NeutrallyCompletedObjective);
};
