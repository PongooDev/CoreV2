#pragma once

#include "Containers/UnrealString.h"
#include "GameFramework/Info.h"
#include "GameFramework/OnlineReplStructs.h"

class APlayerState : public AInfo
{
    DefineUnrealClass(APlayerState, Engine)

public:
    DefineUProperty(FString, PlayerName)
    DefineUProperty(FUniqueNetIdRepl, UniqueId)
    DefineUFunction(GetPlayerName)

    // The player's name. From 1.9 (CL-3757339) the game keeps it in PlayerNamePrivate (+0x418), obfuscated (SetPlayerName
    // 0x23D0D50), and leaves PlayerName empty: there it is GetPlayerName's (0x23C3550). Builds without that function
    // (1.1 to 1.8.2) keep it in PlayerName.
    FString GetPlayerName() const;
};
