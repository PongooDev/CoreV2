#pragma once

#include "UObject/ObjectMacros.h"

#include <cstddef>
#include <cstdint>

class UObjectBase;

struct FUObjectItem
{
    UObjectBase* Object;
    int32_t Flags;
    int32_t ClusterRootIndex;
    int32_t SerialNumber;

    void SetFlags(EInternalObjectFlags FlagsToSet) { Flags |= static_cast<int32_t>(FlagsToSet); }
    void ClearFlags(EInternalObjectFlags FlagsToClear) { Flags &= ~static_cast<int32_t>(FlagsToClear); }
    bool HasAnyFlags(EInternalObjectFlags InFlags) const { return (Flags & static_cast<int32_t>(InFlags)) != 0; }

    void SetPendingKill() { SetFlags(EInternalObjectFlags::PendingKill); }
    void ClearPendingKill() { ClearFlags(EInternalObjectFlags::PendingKill); }
    bool IsPendingKill() const { return HasAnyFlags(EInternalObjectFlags::PendingKill); }

    void SetRootSet() { SetFlags(EInternalObjectFlags::RootSet); }
    void ClearRootSet() { ClearFlags(EInternalObjectFlags::RootSet); }
    bool IsRootSet() const { return HasAnyFlags(EInternalObjectFlags::RootSet); }

    bool IsUnreachable() const { return HasAnyFlags(EInternalObjectFlags::Unreachable); }
};
static_assert(sizeof(FUObjectItem) == 0x18);

class FFixedUObjectArray
{
public:
    FUObjectItem* IndexToObject(int32_t Index) const
    {
        return IsValidIndex(Index) ? &Objects[Index] : nullptr;
    }

    bool IsValidIndex(int32_t Index) const { return Index >= 0 && Index < NumElements; }
    int32_t Num() const { return NumElements; }
    int32_t Capacity() const { return MaxElements; }

private:
    FUObjectItem* Objects;
    int32_t MaxElements;
    int32_t NumElements;
};

class FUObjectArray
{
public:
    FUObjectItem* IndexToObject(int32_t Index) const { return ObjObjects.IndexToObject(Index); }
    int32_t GetObjectArrayNum() const { return ObjObjects.Num(); }
    bool IsDisregardForGC(int32_t Index) const { return Index <= ObjLastNonGCIndex; }

    int32_t ObjFirstGCIndex;
    int32_t ObjLastNonGCIndex;
    int32_t MaxObjectsNotConsideredByGC;
    bool OpenForDisregardForGC;
    FFixedUObjectArray ObjObjects;
};
static_assert(offsetof(FUObjectArray, ObjObjects) == 0x10);
