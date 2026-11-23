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

    // Server, Creative: turns invites and join in progress back on after the match assignment turned them off
    // (OnServerMatchAssignment, 8.20 0x13DCC70, keeps them only on a mesh node), with the game's own
    // UpdateSessionJoinability, which updates the session and sends the flags to the players. Without invites the
    // players' presence has no GameSessionJoinKey (7.00: "SessionKey", SendPresenceUpdate 0x62C120), so friends get no
    // server tile (Core-Backend #411, #375).
    void AllowFriendsToJoin();

private:
    // The identity's OnVerifyAuthComplete delegate, bound in the constructor: the result of ValidateAuth's
    // VerifyAuth(AuthTicket) for each joining player. VerifyResponse is the parsed verify payload (its type isn't in
    // the 21.40 PDB).
    static void OnVerifyAuthCompleteHK(AFortGameSession* This, bool bWasSuccessful, const FAuthTokenMcp& AuthToken, const void* VerifyResponse, const FString& Error);
};
