#pragma once

#include "Engine/EngineBaseTypes.h"
#include "UObject/Object.h"

class FNetworkNotify;
class UWorld;

class UNetDriver : public UObject
{
    DefineUnrealClass(UNetDriver, Engine)
    DefineUProperty(int32_t, MaxInternetClientRate)
    DefineUProperty(int32_t, MaxClientRate)
    DefineUProperty(float, ServerTravelPause)

public:
    bool InitListen(FNetworkNotify* InNotify, FURL& ListenURL, bool bReuseAddressAndPort, FString& Error);
    ENetMode GetNetMode() const;
    void SetWorld(UWorld* InWorld);
};
