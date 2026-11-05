#pragma once

#include "Containers/UnrealString.h"
#include "GameFramework/Info.h"
#include "GameFramework/OnlineReplStructs.h"

class APlayerState : public AInfo
{
    DefineUnrealClass(APlayerState, Engine)

public:
    DefineUProperty(FString, PlayerName)
    DefineUStructProperty(FUniqueNetIdRepl, UniqueId)
    DefineUFunction(GetPlayerName)
    // The replicated name (4.20: PlayerName is only the server's copy; GetPlayerName returns this one).
    DefineUProperty(FString, PlayerNamePrivate)
    static bool HasPlayerNamePrivate() { return Member_PlayerNamePrivate.IsAvailable(); }
    // The player is an AI (a Battle Royale player bot's AAIController player state).
    DefineBitfieldUProperty(bIsABot)
    // The player's unique id in the match, replicated (AGameSession::RegisterPlayer gives a joining player's state the
    // next GetNextPlayerID). 3.5.1's Battle Royale clients list a squadmate on the squad bar only with one (the player
    // state's OnRep_SquadId, 0xA429D0, wants both its own and the local player's nonzero).
    DefineUProperty(int32_t, PlayerId)
    static bool HasPlayerId() { return Member_PlayerId.IsAvailable(); }

    // The player's name. From 1.9 (CL-3757339) the game keeps it in PlayerNamePrivate (+0x418), obfuscated (SetPlayerName
    // 0x23D0D50), and leaves PlayerName empty: there it is GetPlayerName's (0x23C3550). Builds without that function
    // (1.1 to 1.8.2) keep it in PlayerName.
    FString GetPlayerName() const;
};
