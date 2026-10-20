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
};
