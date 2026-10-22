#pragma once

#include "GameFramework/Actor.h"

class UClass;

struct FNavDataConfig
{
    DefineUnrealStruct(FNavDataConfig, Engine)
    DefineUProperty(UClass*, NavigationDataClass)
};

class ANavigationData : public AActor
{
    DefineUnrealClass(ANavigationData, Engine)

public:
    DefineUStructProperty(FNavDataConfig, NavDataConfig)
};
