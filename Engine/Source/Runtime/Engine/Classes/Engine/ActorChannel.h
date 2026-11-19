#pragma once

#include "Engine/Channel.h"

class AActor;
struct FActorDestructionInfo;

class UActorChannel : public UChannel
{
    DefineUnrealClass(UActorChannel, Engine)

public:
    DefineUProperty(AActor*, Actor)
    DefineNativeMember(double, RelevantTime, UActorChannel_RelevantTime)
    // When the actor was last replicated on this channel: the channel's own member up to 2.5; 3.0 keeps it in the
    // connection's map (UNetConnection::LastUpdateTimes), by actor.
    double GetLastUpdateTime() const;
    __declspec(property(get = GetLastUpdateTime)) double LastUpdateTime;
    static bool HasLastUpdateTime() { return Natives::UActorChannel_LastUpdateTime >= 0 || Natives::UNetConnection_LastUpdateTimes >= 0; }
    DefineNativeBitfield(bForceCompareProperties, UActorChannel_Bits, 1u << 1)

    AActor* GetActor() { return Actor; }

    // UE 4.22's signature: the bits written for the actor, 0 when nothing was sent. 3.0 and older (UE 4.20's bool) give
    // 1 or 0 here (Natives::UActorChannel_bReplicateActorReturnsBits).
    int64_t ReplicateActor();
    void SetChannelActor(AActor* InActor);
    void SetChannelActorForDestroy(FActorDestructionInfo* DestructInfo);
};
