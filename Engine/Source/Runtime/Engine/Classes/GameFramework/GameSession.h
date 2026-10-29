#pragma once

#include "GameFramework/Info.h"

class AGameSession : public AInfo
{
    DefineUnrealClass(AGameSession, Engine)

public:
    // The players ApproveLogin lets in ("Server full." past it, AtCapacity).
    DefineUProperty(int32_t, MaxPlayers)

    static bool HasMaxPlayers() { return Member_MaxPlayers.IsAvailable(); }
};
