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

enum { RELIABLE_BUFFER = 256 };
enum { MAX_PACKETID = 16384 };
enum { MAX_CHSEQUENCE = 1024 };
enum { MAX_BUNCH_HEADER_BITS = 64 };
enum { MAX_PACKET_HEADER_BITS = 15 };
enum { MAX_PACKET_TRAILER_BITS = 1 };

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
    enum { MAX_CHANNELS = 10240 };

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
    DefineNativeArray(UChannel*, Channels, UNetConnection_Channels)
    using FActorChannelMap = TMap<TWeakObjectPtr<AActor>, UActorChannel*>;
    using FDormantReplicatorMap = TMap<TWeakObjectPtr<UObject>, TSharedPtr<FObjectReplicator>>;
    DefineNativeObject(FActorChannelMap, ActorChannels, UNetConnection_ActorChannels)
    DefineNativeObject(FDormantReplicatorMap, DormantReplicatorMap, UNetConnection_DormantReplicatorMap)
    DefineNativeObject(TSet<FNetworkGUID>, DestroyedStartupOrDormantActors, UNetConnection_DestroyedStartupOrDormantActors)
    DefineNativeMember(FName, ClientWorldPackageName, UNetConnection_ClientWorldPackageName)

    // ClientVisibleLevelNames: a TArray<FName> on 1.1 and 1.2, a TSet<FName> on 1.7.2 and 1.8
    // (Natives::UNetConnection_ClientVisibleLevelNamesIsSet).
    bool IsClientLevelVisible(FName LevelName) const
    {
        const void* VisibleLevelNames = reinterpret_cast<const uint8_t*>(this) + Natives::UNetConnection_ClientVisibleLevelNames;
        return Natives::UNetConnection_ClientVisibleLevelNamesIsSet
            ? static_cast<const TSet<FName>*>(VisibleLevelNames)->Contains(LevelName)
            : static_cast<const TArray<FName>*>(VisibleLevelNames)->Contains(LevelName);
    }

    UChannel* CreateChannel(EChannelType Type, bool bOpenedLocally, int32_t ChannelIndex = -1);
    int32_t IsNetReady(bool Saturate);
    bool ClientHasInitializedLevelFor(const UObject* TestObject) const;
    void FlushDormancy(AActor* Actor);
};

class UChildConnection : public UNetConnection
{
    DefineUnrealClass(UChildConnection, Engine)

public:
    DefineUProperty(UNetConnection*, Parent)
};
