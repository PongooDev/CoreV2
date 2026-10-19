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
    DefineNativeMember(double, LastUpdateTime, UActorChannel_LastUpdateTime)
    DefineNativeBitfield(bForceCompareProperties, UActorChannel_Bits, 1u << 1)

    AActor* GetActor() { return Actor; }

    bool ReplicateActor();
    void SetChannelActor(AActor* InActor);
    void SetChannelActorForDestroy(FActorDestructionInfo* DestructInfo);
};
