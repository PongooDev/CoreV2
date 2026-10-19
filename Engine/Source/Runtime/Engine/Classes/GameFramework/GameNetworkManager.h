#pragma once

#include "GameFramework/Actor.h"

class AGameNetworkManager : public AActor
{
    DefineUnrealClass(AGameNetworkManager, Engine)

public:
    bool IsInLowBandwidthMode();
};
