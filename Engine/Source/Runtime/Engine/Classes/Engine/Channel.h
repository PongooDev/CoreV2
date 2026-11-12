#pragma once

#include "ObjectLayout.h"
#include "UObject/Object.h"

class FOutBunch;
class UNetConnection;

enum EChannelType
{
    CHTYPE_None = 0,
    CHTYPE_Control = 1,
    CHTYPE_Actor = 2,
    CHTYPE_File = 3,
    CHTYPE_Voice = 4,
    CHTYPE_MAX = 8,
};

// 6.10 (UE 4.22's networking) names its channels, and UNetConnection::CreateChannelByName takes these in place of
// bOpenedLocally. The game's own callers pass 2 for a channel they open (lea r8d, [r9+3] with r9d -1, CL-4480234
// 0x26F9CC7) and 1 for one the other side opened (UNetConnection::ReceivedPacket, 0x2702013).
enum class EChannelCreateFlags : uint32_t
{
    None = (1 << 0),
    OpenedLocally = (1 << 1),
};

struct FPacketIdRange
{
    int32_t First = -1;
    int32_t Last = -1;
};

class UChannel : public UObject
{
    DefineUnrealClass(UChannel, Engine)

public:
    DefineUProperty(UNetConnection*, Connection)
    DefineNativeBitfield(Closing, UChannel_Bits, 1u << 1)
    DefineNativeBitfield(Dormant, UChannel_Bits, 1u << 2)
    DefineNativeBitfield(bPendingDormancy, UChannel_Bits, 1u << 7)
    DefineNativeBitfield(bPausedUntilReliableACK, UChannel_Bits, 1u << 8)
    DefineNativeMember(int32_t, ChIndex, UChannel_ChIndex)
    DefineNativeMember(int32_t, NumOutRec, UChannel_NumOutRec)

    void Close();
    FPacketIdRange SendBunch(FOutBunch* Bunch, bool Merge);
    void StartBecomingDormant();
    int32_t IsNetReady(bool Saturate);
};
