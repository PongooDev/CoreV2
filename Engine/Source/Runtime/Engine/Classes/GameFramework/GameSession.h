#pragma once

#include "GameFramework/Info.h"
#include "Natives.h"
#include "ObjectLayout.h"

class APlayerController;
class FText;

class AGameSession : public AInfo
{
    DefineUnrealClass(AGameSession, Engine)

public:
    // The players ApproveLogin lets in ("Server full." past it, AtCapacity).
    DefineUProperty(int32_t, MaxPlayers)
    DefineUProperty(int32_t, MaxSpectators)

    static bool HasMaxPlayers() { return Member_MaxPlayers.IsAvailable(); }

    // Server: the engine's kick, through its virtual (Fortnite's AFortGameSession override logs "KickPlayer %s Reason %s"
    // and kicks through the engine's: the client's ClientWasKicked with KickReason, the controller destroyed). False
    // when the slot isn't known (Finder::ResolveGameSessionKickLive) or the kick didn't happen.
    bool KickPlayer(APlayerController* KickedPlayer, const FText& KickReason)
    {
        return Natives::AGameSession_KickPlayerIndex >= 0 && CallVirtual<bool>(this, Natives::AGameSession_KickPlayerIndex, KickedPlayer, &KickReason);
    }
};
