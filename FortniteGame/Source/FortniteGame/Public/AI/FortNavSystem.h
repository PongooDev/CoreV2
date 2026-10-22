#pragma once

#include "AI/Navigation/NavigationSystem.h"

class AFortNavigationGraph;

class UFortNavSystem : public UNavigationSystem
{
    DefineUnrealClass(UFortNavSystem, FortniteGame)
    DefineUFunction(OnNavDataRegistered)

public:
    DefineUProperty(AFortNavigationGraph*, NavGraphData)

    void OnNavDataRegistered(ANavigationData* NavData);
};
