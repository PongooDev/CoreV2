#pragma once

#include "GameFramework/Actor.h"

class UClass;

struct FNavDataConfig
{
    DefineUnrealStruct(FNavDataConfig, Engine)
    DefineUProperty(UClass*, NavigationDataClass)
    // FNavAgentProperties': the agent's capsule.
    DefineUProperty(float, AgentRadius)
    DefineUProperty(float, AgentHeight)
    DefineUProperty(FName, Name)
};

class ANavigationData : public AActor
{
    DefineUnrealClass(ANavigationData, Engine)

public:
    DefineUStructProperty(FNavDataConfig, NavDataConfig)
    // ERuntimeGenerationType: 0 Static (no generator in a game world), 1 DynamicModifiersOnly, 2 Dynamic. Read when the
    // navigation system registers the data and makes its generator.
    DefineUProperty(uint8_t, RuntimeGeneration)
    static bool HasRuntimeGeneration() { return Member_RuntimeGeneration.IsAvailable(); }
};
