#pragma once

#include "UObject/Object.h"

// The params of the "MissionForceSuccess" mission event, which UFortCheatManager::MissionForcePrimarySuccess sends to the
// primary mission: how much of it counts as done.
class UFortMissionForceSuccessParams : public UObject
{
    DefineUnrealClass(UFortMissionForceSuccessParams, FortniteGame)
    DefineUFunction(SetParams)

public:
    UFortMissionForceSuccessParams* SetParams(float FractionCompleted);
};
