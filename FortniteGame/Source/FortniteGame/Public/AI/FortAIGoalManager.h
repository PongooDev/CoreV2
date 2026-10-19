#pragma once

#include "GameFramework/Actor.h"

class AFortAIGoalManager : public AActor
{
    DefineUnrealClass(AFortAIGoalManager, FortniteGame)

public:
    static AFortAIGoalManager* GetCurrent(const UObject* WorldContextObject);
};
