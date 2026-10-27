#pragma once

#include "Containers/Array.h"
#include "GameFramework/Info.h"

class APlayerState;

class AGameStateBase : public AInfo
{
    DefineUnrealClass(AGameStateBase, Engine)

public:
    // Every connected player's state.
    DefineUProperty(TArray<APlayerState*>, PlayerArray)
};
