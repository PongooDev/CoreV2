#include "pch.h"
#include "Engine/NetConnection.h"
#include "Natives.h"

// 6.10 (CL-4480234) has UE 4.22's named channels: UNetConnection::CreateChannelByName (0x26EDF50) is the only function
// that makes one, and the build keeps no CreateChannel by type. UE 4.22's own CreateChannel is a deprecated wrapper that
// turns the type into its name and forwards, which is what this does there. The names are the ones the build's
// UNetConnection::ReceivedPacket (0x27005F0) gives an old-format bunch's type: Control 0FFh, Actor 66h, Voice 100h.
UChannel* UNetConnection::CreateChannel(EChannelType Type, bool bOpenedLocally, int32_t ChannelIndex)
{
    if (Natives::UNetConnection_CreateChannelByName)
    {
        FName ChName = NAME_None;
        switch (Type)
        {
        case CHTYPE_Control:
            ChName = NAME_Control;
            break;
        case CHTYPE_Actor:
            ChName = NAME_Actor;
            break;
        case CHTYPE_Voice:
            ChName = NAME_Voice;
            break;
        default:
            break;
        }
        return CreateChannelByName(ChName, bOpenedLocally ? EChannelCreateFlags::OpenedLocally : EChannelCreateFlags::None, ChannelIndex);
    }
    return Natives::UNetConnection_CreateChannel(this, Type, bOpenedLocally, ChannelIndex);
}

UChannel* UNetConnection::CreateChannelByName(const FName& ChName, EChannelCreateFlags CreateFlags, int32_t ChannelIndex)
{
    return Natives::UNetConnection_CreateChannelByName(this, ChName, static_cast<uint32_t>(CreateFlags), ChannelIndex);
}

int32_t UNetConnection::IsNetReady(bool Saturate)
{
    return CallVirtual<int32_t>(this, Natives::UNetConnection_IsNetReadyIndex, Saturate);
}

bool UNetConnection::ClientHasInitializedLevelFor(const UObject* TestObject) const
{
    return CallVirtual<bool>(this, Natives::UNetConnection_ClientHasInitializedLevelForIndex, TestObject);
}

void UNetConnection::FlushDormancy(AActor* Actor)
{
    CallVirtual<void>(this, Natives::UNetConnection_FlushDormancyIndex, Actor);
}
