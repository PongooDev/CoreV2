#pragma once

#include "GameFramework/Actor.h"
#include "GameFramework/OnlineReplStructs.h"

class AFortLobbyBeaconHost : public AActor
{
    DefineUnrealClass(AFortLobbyBeaconHost, FortniteGame)

public:
    void SetWorldRecordLoaded(bool bInWorldRecordLoaded);
};

// A player's state on the lobby beacon, which the client joins before the game (AFortLobbyBeaconPlayerState). Its
// ServerSetPartyOwner (1.7.2 0x6E2EB0) stores the client's party leader in PartyOwnerUniqueId.
class ALobbyBeaconPlayerState : public AActor
{
    DefineUnrealClass(ALobbyBeaconPlayerState, Lobby)

public:
    DefineUProperty(FUniqueNetIdRepl, UniqueId)
    DefineUProperty(FUniqueNetIdRepl, PartyOwnerUniqueId)
};
