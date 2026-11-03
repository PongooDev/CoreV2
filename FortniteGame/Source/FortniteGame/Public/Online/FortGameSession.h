#pragma once

#include "Containers/UnrealString.h"
#include "GameFramework/GameSession.h"

class AFortLobbyBeaconHost;
class APartyBeaconHost;
struct FAuthTokenMcp;

class AFortGameSession : public AGameSession
{
    DefineUnrealClass(AFortGameSession, FortniteGame)

public:
    DefineUProperty(AFortLobbyBeaconHost*, LobbyBeaconHost)
    // An AFortPartyBeaconHost (Online/FortPartyBeaconHost.h).
    DefineUProperty(APartyBeaconHost*, ReservationBeaconHost)

    static bool HasReservationBeacon() { return Member_ReservationBeaconHost.IsAvailable(); }

    static void HookVTables();

private:
    // The identity's OnVerifyAuthComplete delegate, bound in the constructor: the result of ValidateAuth's
    // VerifyAuth(AuthTicket) for each joining player. VerifyResponse is the parsed verify payload (its type isn't in
    // the 21.40 PDB).
    static void OnVerifyAuthCompleteHK(AFortGameSession* This, bool bWasSuccessful, const FAuthTokenMcp& AuthToken, const void* VerifyResponse, const FString& Error);
};
