#pragma once

#include "GameFramework/Actor.h"

class AFortLobbyBeaconHost : public AActor
{
    DefineUnrealClass(AFortLobbyBeaconHost, FortniteGame)

public:
    void SetWorldRecordLoaded(bool bInWorldRecordLoaded);
};
