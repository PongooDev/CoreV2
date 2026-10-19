#include "pch.h"
#include "Engine/NetConnection.h"
#include "Natives.h"

UChannel* UNetConnection::CreateChannel(EChannelType Type, bool bOpenedLocally, int32_t ChannelIndex)
{
    return Natives::UNetConnection_CreateChannel(this, Type, bOpenedLocally, ChannelIndex);
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
