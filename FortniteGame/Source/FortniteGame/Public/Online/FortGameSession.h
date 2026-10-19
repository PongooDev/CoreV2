#pragma once

#include "GameFramework/GameSession.h"

class AFortLobbyBeaconHost;

class AFortGameSession : public AGameSession
{
    DefineUnrealClass(AFortGameSession, FortniteGame)

public:
    DefineUProperty(AFortLobbyBeaconHost*, LobbyBeaconHost)
};
