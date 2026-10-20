#pragma once

#include "UObject/Object.h"

class AFortPlayerController;
class UFortWorldItemDefinition;

class UFortItemCollectedParams : public UObject
{
    DefineUnrealClass(UFortItemCollectedParams, FortniteGame)
    DefineUFunction(SetParams)

public:
    UFortItemCollectedParams* SetParams(UFortWorldItemDefinition* CollectedItemDefinition, AFortPlayerController* CollectedBy, int32_t AmountCollected);

    static void SendItemCollected(AFortPlayerController* CollectedBy, UFortWorldItemDefinition* CollectedItemDefinition, int32_t AmountCollected);
};
