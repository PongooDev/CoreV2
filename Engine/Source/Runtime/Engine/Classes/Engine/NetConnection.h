#pragma once

#include "Containers/Map.h"
#include "Engine/Channel.h"
#include "Engine/Player.h"
#include "Misc/NetworkGuid.h"
#include "Templates/SharedPointer.h"
#include "UObject/WeakObjectPtr.h"

class AActor;
class FObjectReplicator;
class UActorChannel;
class UChildConnection;
class UNetDriver;
class UPackageMap;

enum EConnectionState
{
    USOCK_Invalid = 0,
    USOCK_Closed = 1,
    USOCK_Pending = 2,
    USOCK_Open = 3,
};

class UNetConnection : public UPlayer
{
    DefineUnrealClass(UNetConnection, Engine)

public:
    DefineUProperty(TArray<UChildConnection*>, Children)
    DefineUProperty(UNetDriver*, Driver)
    DefineUProperty(UPackageMap*, PackageMap)
    DefineUProperty(TArray<AActor*>, SentTemporaries)
    DefineUProperty(AActor*, ViewTarget)
    DefineUProperty(AActor*, OwningActor)
    DefineBitfieldUProperty(InternalAck)
    DefineUProperty(double, LastReceiveTime)
    DefineNativeMember(EConnectionState, State, UNetConnection_State)
    DefineNativeMember(int32_t, QueuedBits, UNetConnection_QueuedBits)
    DefineNativeMember(int32_t, TickCount, UNetConnection_TickCount)
    DefineNativeMember(bool, TimeSensitive, UNetConnection_TimeSensitive)
    using FActorChannelMap = TMap<TWeakObjectPtr<AActor>, UActorChannel*>;
    using FDormantReplicatorMap = TMap<TWeakObjectPtr<UObject>, TSharedPtr<FObjectReplicator>>;
    DefineNativeObject(FActorChannelMap, ActorChannels, UNetConnection_ActorChannels)
    DefineNativeObject(FDormantReplicatorMap, DormantReplicatorMap, UNetConnection_DormantReplicatorMap)
    DefineNativeObject(TSet<FNetworkGUID>, DestroyedStartupOrDormantActors, UNetConnection_DestroyedStartupOrDormantActors)
    DefineNativeMember(FName, ClientWorldPackageName, UNetConnection_ClientWorldPackageName)
    DefineNativeMember(TArray<FName>, ClientVisibleLevelNames, UNetConnection_ClientVisibleLevelNames)

    UChannel* CreateChannel(EChannelType Type, bool bOpenedLocally, int32_t ChannelIndex = -1);
    bool IsNetReady(bool Saturate);
    bool ClientHasInitializedLevelFor(const UObject* TestObject) const;
    void FlushDormancy(AActor* Actor);
};

class UChildConnection : public UNetConnection
{
    DefineUnrealClass(UChildConnection, Engine)

public:
    DefineUProperty(UNetConnection*, Parent)
};
