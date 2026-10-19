#pragma once

#include "Containers/Set.h"
#include "ObjectLayout.h"
#include "Templates/SharedPointer.h"
#include "UObject/WeakObjectPtr.h"

class AActor;
class UNetConnection;

struct FNetworkObjectInfo
{
    AActor* Actor;
    double NextUpdateTime;
    double LastNetReplicateTime;
    float OptimalNetUpdateDelta;
    float LastNetUpdateTime;
    uint32_t bPendingNetUpdate : 1;
    uint32_t bDirtyForReplay : 1;
    TSet<TWeakObjectPtr<UNetConnection>> DormantConnections;
    TSet<TWeakObjectPtr<UNetConnection>> RecentlyDormantConnections;
};

static_assert(offsetof(FNetworkObjectInfo, NextUpdateTime) == 0x8);
static_assert(offsetof(FNetworkObjectInfo, OptimalNetUpdateDelta) == 0x18);
static_assert(offsetof(FNetworkObjectInfo, LastNetUpdateTime) == 0x1C);
static_assert(offsetof(FNetworkObjectInfo, DormantConnections) == 0x28);
static_assert(offsetof(FNetworkObjectInfo, RecentlyDormantConnections) == 0x78);
static_assert(sizeof(FNetworkObjectInfo) == 0xC8);

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
