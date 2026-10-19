#include "pch.h"
#include "Engine/ActorChannel.h"
#include "Engine/NetConnection.h"
#include "Engine/NetDriver.h"
#include "Engine/PackageMapClient.h"
#include "EngineLogs.h"
#include "Natives.h"
#include "Net/DataBunch.h"

DEFINE_LOG_CATEGORY(LogNet);
DEFINE_LOG_CATEGORY(LogRep);
DEFINE_LOG_CATEGORY(LogNetPlayerMovement);
DEFINE_LOG_CATEGORY(LogNetTraffic);
DEFINE_LOG_CATEGORY(LogRepTraffic);
DEFINE_LOG_CATEGORY(LogNetDormancy);
DEFINE_LOG_CATEGORY(LogNetFastTArray);
DEFINE_LOG_CATEGORY(LogSecurity);

void UChannel::Close()
{
    CallVirtual<void>(this, Natives::UChannel_CloseIndex);
}

FPacketIdRange UChannel::SendBunch(FOutBunch* Bunch, bool Merge)
{
    FPacketIdRange Range;
    CallVirtual<FPacketIdRange*>(this, Natives::UChannel_SendBunchIndex, &Range, Bunch, Merge);
    return Range;
}

void UChannel::StartBecomingDormant()
{
    CallVirtual<void>(this, Natives::UChannel_StartBecomingDormantIndex);
}

int32_t UChannel::IsNetReady(bool Saturate)
{
    if (NumOutRec >= RELIABLE_BUFFER - 1)
    {
        return 0;
    }

    return Connection->IsNetReady(Saturate);
}

bool UActorChannel::ReplicateActor()
{
    return Natives::UActorChannel_ReplicateActor(this);
}

void UActorChannel::SetChannelActor(AActor* InActor)
{
    Natives::UActorChannel_SetChannelActor(this, InActor);
}

void UActorChannel::SetChannelActorForDestroy(FActorDestructionInfo* DestructInfo)
{
    check(Connection->Channels[ChIndex] == this);

    if (Closing || (Connection->State != USOCK_Open && Connection->State != USOCK_Pending))
    {
        return;
    }

    FOutBunch CloseBunch(this, true);
    check(!CloseBunch.IsError());
    check(CloseBunch.bClose);
    CloseBunch.bReliable = 1;
    CloseBunch.bDormant = 0;

    Connection->PackageMap->WriteObject(CloseBunch, DestructInfo->ObjOuter.Get(), DestructInfo->NetGUID, DestructInfo->PathName);

    UE_LOG(LogNetTraffic, Log, TEXT("SetChannelActorForDestroy: Channel %d. NetGUID <%s> Path: %s. Bits: %d"), ChIndex, *DestructInfo->NetGUID.ToString(), *DestructInfo->PathName, CloseBunch.GetNumBits());
    UE_LOG(LogNetDormancy, Verbose, TEXT("SetChannelActorForDestroy: Channel %d. NetGUID <%s> Path: %s. Bits: %d"), ChIndex, *DestructInfo->NetGUID.ToString(), *DestructInfo->PathName, CloseBunch.GetNumBits());

    SendBunch(&CloseBunch, false);
}
