#pragma once

#include "Containers/Set.h"
#include "ObjectLayout.h"
#include "Templates/SharedPointer.h"
#include "UObject/WeakObjectPtr.h"

class AActor;
class UNetConnection;

// The engine allocates these (0xC8 bytes up to 1.7.2, 0xD0 from 1.8); Core only reads and updates them in place. Every
// member after Actor is at the offset ResolveReplication found: 1.8 puts a TWeakObjectPtr<AActor> right after Actor.
struct FNetworkObjectInfo
{
    using FConnectionSet = TSet<TWeakObjectPtr<UNetConnection>>;

    AActor* Actor;

    DefineNativeMember(double, NextUpdateTime, FNetworkObjectInfo_NextUpdateTime)
    DefineNativeMember(double, LastNetReplicateTime, FNetworkObjectInfo_LastNetReplicateTime)
    DefineNativeMember(float, OptimalNetUpdateDelta, FNetworkObjectInfo_OptimalNetUpdateDelta)
    DefineNativeMember(float, LastNetUpdateTime, FNetworkObjectInfo_LastNetUpdateTime)
    DefineNativeBitfield(bPendingNetUpdate, FNetworkObjectInfo_Flags, 0x1)
    DefineNativeBitfield(bDirtyForReplay, FNetworkObjectInfo_Flags, 0x2)
    DefineNativeObject(FConnectionSet, DormantConnections, FNetworkObjectInfo_DormantConnections)
    DefineNativeObject(FConnectionSet, RecentlyDormantConnections, FNetworkObjectInfo_RecentlyDormantConnections)

    FNetworkObjectInfo() = delete;
};

struct FNetworkObjectKeyFuncs
{
    using KeyInitType = AActor*;
    static constexpr bool bAllowDuplicateKeys = false;

    static KeyInitType GetSetKey(const TSharedPtr<FNetworkObjectInfo>& Element)
    {
        return Element.Get()->Actor;
    }

    static bool Matches(KeyInitType A, KeyInitType B)
    {
        return A == B;
    }

    static uint32_t GetKeyHash(KeyInitType Key)
    {
        return GetTypeHash(Key);
    }
};

class FNetworkObjectList
{
public:
    using FNetworkObjectSet = TSet<TSharedPtr<FNetworkObjectInfo>, FNetworkObjectKeyFuncs>;

    void Remove(AActor* const Actor);

    const FNetworkObjectSet& GetAllObjects() const
    {
        return AllNetworkObjects;
    }

    const FNetworkObjectSet& GetActiveObjects() const
    {
        return ActiveNetworkObjects;
    }

    FNetworkObjectSet& GetActiveObjects()
    {
        return ActiveNetworkObjects;
    }

private:
    FNetworkObjectSet AllNetworkObjects;
    FNetworkObjectSet ActiveNetworkObjects;
};

static_assert(sizeof(FNetworkObjectList::FNetworkObjectSet) == 0x50);
