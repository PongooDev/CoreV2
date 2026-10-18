#pragma once

#include <cstddef>
#include <cstdint>

class UObjectBase;

struct FUObjectItem
{
    UObjectBase* Object;
    int32_t Flags;
    int32_t ClusterRootIndex;
    int32_t SerialNumber;
    int32_t Padding;
};
static_assert(sizeof(FUObjectItem) == 0x18);

struct FFixedUObjectArray
{
    FUObjectItem* Objects;
    int32_t MaxElements;
    int32_t NumElements;

    FUObjectItem* GetItem(int32_t Index) const
    {
        return Index >= 0 && Index < NumElements ? Objects + Index : nullptr;
    }
};

struct FUObjectArray
{
    int32_t ObjFirstGCIndex;
    int32_t ObjLastNonGCIndex;
    int32_t MaxObjectsNotConsideredByGC;
    bool OpenForDisregardForGC;
    FFixedUObjectArray ObjObjects;

    int32_t Num() const
    {
        return ObjObjects.NumElements;
    }

    FUObjectItem* IndexToObject(int32_t Index) const
    {
        return ObjObjects.GetItem(Index);
    }
};
static_assert(offsetof(FUObjectArray, ObjObjects) == 0x10);
