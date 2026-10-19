#pragma once

#include "Containers/Map.h"
#include "Engine/EngineBaseTypes.h"
#include "Math/Vector.h"
#include "Misc/NetworkGuid.h"
#include "ObjectLayout.h"
#include "Templates/SharedPointer.h"
#include "UObject/Object.h"
#include "UObject/WeakObjectPtr.h"

class AActor;
class FNetGUIDCache;
class FNetworkNotify;
class FNetworkObjectList;
class UActorChannel;
class UNetConnection;
class UWorld;
struct FNetViewer;
struct FNetworkObjectInfo;

struct FActorDestructionInfo
{
    TWeakObjectPtr<UObject> ObjOuter;
    FVector DestroyedPosition;
    FNetworkGUID NetGUID;
    FString PathName;
    FName StreamingLevelName;
};

static_assert(offsetof(FActorDestructionInfo, NetGUID) == 0x14);
static_assert(offsetof(FActorDestructionInfo, PathName) == 0x18);
static_assert(sizeof(FActorDestructionInfo) == 0x30);

struct FActorPriority
{
    int32_t Priority = 0;
    FNetworkObjectInfo* ActorInfo = nullptr;
    UActorChannel* Channel = nullptr;
    FActorDestructionInfo* DestructionInfo = nullptr;

    FActorPriority() = default;
    FActorPriority(UNetConnection* InConnection, UActorChannel* InChannel, FNetworkObjectInfo* InActorInfo, const TArray<FNetViewer>& Viewers, bool bLowBandwidth);
    FActorPriority(UNetConnection* InConnection, FActorDestructionInfo* DestructInfo, const TArray<FNetViewer>& Viewers);
};

struct FCompareFActorPriority
{
    bool operator()(const FActorPriority& A, const FActorPriority& B) const
    {
        return B.Priority < A.Priority;
    }
};

class UNetDriver : public UObject
{
    DefineUnrealClass(UNetDriver, Engine)

public:
    DefineUProperty(int32_t, MaxInternetClientRate)
    DefineUProperty(int32_t, MaxClientRate)
    DefineUProperty(float, ServerTravelPause)
    DefineUProperty(float, SpawnPrioritySeconds)
    DefineUProperty(float, RelevantTimeout)
    DefineUProperty(UNetConnection*, ServerConnection)
    DefineUProperty(TArray<UNetConnection*>, ClientConnections)
    DefineUProperty(UWorld*, World)
    DefineUProperty(FName, NetDriverName)
    DefineUProperty(float, Time)
    DefineNativeMember(bool, bIsPeer, UNetDriver_bIsPeer)
    DefineNativeMember(TSharedPtr<FNetGUIDCache>, GuidCache, UNetDriver_GuidCache)
    DefineNativeMember(int32_t, NetTag, UNetDriver_NetTag)
    DefineNativeMember(bool, DebugRelevantActors, UNetDriver_DebugRelevantActors)
    DefineNativeMember(TArray<TWeakObjectPtr<AActor>>, LastPrioritizedActors, UNetDriver_LastPrioritizedActors)
    DefineNativeMember(TArray<TWeakObjectPtr<AActor>>, LastRelevantActors, UNetDriver_LastRelevantActors)
    DefineNativeMember(TArray<TWeakObjectPtr<AActor>>, LastSentActors, UNetDriver_LastSentActors)
    DefineNativeMember(TArray<TWeakObjectPtr<AActor>>, LastNonRelevantActors, UNetDriver_LastNonRelevantActors)
    using FDestroyedActorMap = TMap<FNetworkGUID, FActorDestructionInfo>;
    DefineNativeObject(FDestroyedActorMap, DestroyedStartupOrDormantActors, UNetDriver_DestroyedStartupOrDormantActors)
    DefineNativeMember(uint32_t, ReplicationFrame, UNetDriver_ReplicationFrame)
    DefineNativeMember(TSharedPtr<FNetworkObjectList>, NetworkObjects, UNetDriver_NetworkObjects)

    bool InitListen(FNetworkNotify* InNotify, FURL& ListenURL, bool bReuseAddressAndPort, FString& Error);
    ENetMode GetNetMode() const;
    void SetWorld(UWorld* InWorld);

    FNetworkObjectList& GetNetworkObjectList() { return *NetworkObjects; }
    const FNetworkObjectList& GetNetworkObjectList() const { return *NetworkObjects; }

    FString GetDescription()
    {
        return FString::Printf(TEXT("%s %s%s"), *NetDriverName.ToString(), *GetName(), bIsPeer ? TEXT("(PEER)") : TEXT(""));
    }

    bool IsServer() const;
    bool IsLevelInitializedForActor(const AActor* InActor, const UNetConnection* InConnection) const;
    static bool IsAdaptiveNetUpdateFrequencyEnabled();
    void TickFlush(float DeltaSeconds);
    int32_t ServerReplicateActors(float DeltaSeconds);
    void FlushActorDormancy(AActor* Actor);
    void PrintDebugRelevantActors();

    static void Hook();

#if WITH_SERVER_CODE
private:
    int32_t ServerReplicateActors_PrepConnections(const float DeltaSeconds);
    void ServerReplicateActors_BuildConsiderList(TArray<FNetworkObjectInfo*>& OutConsiderList, const float ServerTickTime);
    int32_t ServerReplicateActors_PrioritizeActors(UNetConnection* Connection, const TArray<FNetViewer>& ConnectionViewers, const TArray<FNetworkObjectInfo*> ConsiderList, const bool bCPUSaturated, FActorPriority*& OutPriorityList, FActorPriority**& OutPriorityActors);
    int32_t ServerReplicateActors_ProcessPrioritizedActors(UNetConnection* Connection, const TArray<FNetViewer>& ConnectionViewers, FActorPriority** PriorityActors, const int32_t FinalSortedCount, int32_t& OutUpdated);
#endif

    static inline void (*TickFlushOG)(UNetDriver* This, float DeltaSeconds) = nullptr;
    static void TickFlushHK(UNetDriver* This, float DeltaSeconds);
    static int32_t ServerReplicateActorsHK(UNetDriver* This, float DeltaSeconds);
};
