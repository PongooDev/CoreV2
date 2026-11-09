#include "pch.h"
#include "Engine/NetSerialization.h"
#include "Natives.h"

void FFastArraySerializer::MarkArrayDirty()
{
    if (Natives::FFastArraySerializer_MarkArrayDirty)
    {
        Natives::FFastArraySerializer_MarkArrayDirty(this);
        return;
    }

    // 5.0 (UE 4.21) inlines it everywhere: ItemMap.Reset(); IncrementArrayReplicationKey(); CachedNumItems = INDEX_NONE;
    // CachedNumItemsToConsiderForWriting = INDEX_NONE. ArrayReplicationKey follows IDCounter.
    Natives::FFastArraySerializer_ItemMapReset(this);
    int32_t& ArrayReplicationKey = *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(this) + Natives::FFastArraySerializer_IDCounter + sizeof(int32_t));
    ArrayReplicationKey++;
    if (ArrayReplicationKey == INDEX_NONE)
    {
        ArrayReplicationKey++;
    }
    int32_t* const CachedNumItems = reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(this) + Natives::FFastArraySerializer_CachedNumItems);
    CachedNumItems[0] = INDEX_NONE;
    CachedNumItems[1] = INDEX_NONE;
}
