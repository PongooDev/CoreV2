#pragma once

#include "ObjectLayout.h"
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

// The engine's FUObjectArray, whose members start ObjectLayout's FUObjectArray_Members bytes in: 1.10's has 0x344 bytes
// in front of ObjFirstGCIndex (FUObjectArray::AllocateObjectPool, 0xDD8E20, stores MaxObjectsNotConsideredByGC at +0x34C),
// which puts ObjObjects, 8-aligned after OpenForDisregardForGC, at +0x358.
class FUObjectArray
{
public:
    FUObjectItem* IndexToObject(int32_t Index) const { return ObjObjects().IndexToObject(Index); }
    int32_t GetObjectArrayNum() const { return ObjObjects().Num(); }
    bool IsDisregardForGC(int32_t Index) const { return Index <= ObjLastNonGCIndex(); }

private:
    // ObjFirstGCIndex, ObjLastNonGCIndex, MaxObjectsNotConsideredByGC, OpenForDisregardForGC, then ObjObjects.
    uintptr_t Members() const { return reinterpret_cast<uintptr_t>(this) + ObjectLayout::Current.FUObjectArray_Members; }
    int32_t ObjLastNonGCIndex() const { return *reinterpret_cast<const int32_t*>(Members() + sizeof(int32_t)); }
    const FFixedUObjectArray& ObjObjects() const { return *reinterpret_cast<const FFixedUObjectArray*>((Members() + 3 * sizeof(int32_t) + sizeof(bool) + alignof(FFixedUObjectArray) - 1) & ~(alignof(FFixedUObjectArray) - 1)); }
};
