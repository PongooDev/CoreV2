#pragma once

#include "GameFramework/GameSession.h"

class AFortLobbyBeaconHost;
class APartyBeaconHost;

class AFortGameSession : public AGameSession
{
    DefineUnrealClass(AFortGameSession, FortniteGame)

public:
    DefineUProperty(AFortLobbyBeaconHost*, LobbyBeaconHost)
    // An AFortPartyBeaconHost (Online/FortPartyBeaconHost.h).
    DefineUProperty(APartyBeaconHost*, ReservationBeaconHost)

    static bool HasReservationBeacon() { return Member_ReservationBeaconHost.IsAvailable(); }
};
