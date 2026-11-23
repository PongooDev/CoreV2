#pragma once

#include "GameFramework/Info.h"
#include "UObject/NameTypes.h"
#include "Natives.h"
#include "ObjectLayout.h"

class APlayerController;
class FText;

// How a session can be joined (Epic's in Net/OnlineEngineInterface.h; this layout from 7.40 to 8.30, 8.20's
// UOnlineEngineInterfaceImpl::GetSessionJoinability 0x645330 and AGameSession's 0x29D5600).
struct FJoinabilitySettings
{
    FName SessionName;
    bool bPublicSearchable = false;
    bool bAllowInvites = false;
    bool bJoinViaPresence = false;
    bool bJoinViaPresenceFriendsOnly = false;
    int32_t MaxPlayers = 0;
    int32_t MaxPartySize = 0;
};

class AGameSession : public AInfo
{
    DefineUnrealClass(AGameSession, Engine)

public:
    // The players ApproveLogin lets in ("Server full." past it, AtCapacity).
    DefineUProperty(int32_t, MaxPlayers)
    DefineUProperty(int32_t, MaxSpectators)

    static bool HasMaxPlayers() { return Member_MaxPlayers.IsAvailable(); }
    static ptrdiff_t MaxPlayersOffset() { return HasMaxPlayers() ? Member_MaxPlayers.GetOffsetChecked() : -1; }

    // Server: the engine's kick, through its virtual (Fortnite's AFortGameSession override logs "KickPlayer %s Reason %s"
    // and kicks through the engine's: the client's ClientWasKicked with KickReason, the controller destroyed). False
    // when the slot isn't known (Finder::ResolveGameSessionKickLive) or the kick didn't happen.
    bool KickPlayer(APlayerController* KickedPlayer, const FText& KickReason)
    {
        return Natives::AGameSession_KickPlayerIndex >= 0 && CallVirtual<bool>(this, Natives::AGameSession_KickPlayerIndex, KickedPlayer, &KickReason);
    }

    // Through their virtuals (Finder::ResolveSessionJoinabilityLive). AFortGameSession's UpdateSessionJoinability also
    // sends the new flags to every player (UpdateClientsSessionJoinability, ClientSetInviteFlags).
    bool GetSessionJoinability(FName InSessionName, FJoinabilitySettings& OutSettings)
    {
        return Natives::AGameSession_GetSessionJoinabilityIndex >= 0 && CallVirtual<bool>(this, Natives::AGameSession_GetSessionJoinabilityIndex, InSessionName, &OutSettings);
    }

    void UpdateSessionJoinability(FName InSessionName, bool bPublicSearchable, bool bAllowInvites, bool bJoinViaPresence, bool bJoinViaPresenceFriendsOnly)
    {
        if (Natives::AGameSession_UpdateSessionJoinabilityIndex >= 0)
        {
            CallVirtual<void>(this, Natives::AGameSession_UpdateSessionJoinabilityIndex, InSessionName, bPublicSearchable, bAllowInvites, bJoinViaPresence, bJoinViaPresenceFriendsOnly);
        }
    }
};
