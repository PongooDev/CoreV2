#include "pch.h"
#include "Items/FortInventory.h"
#include "ReflectionParams.h"
#include "UObject/Class.h"

FFortItemEntry* FFortItemList::FindReplicatedEntry(const FGuid& ItemGuid)
{
    TStructArray<FFortItemEntry>& Entries = ReplicatedEntries;
    for (int32_t Index = 0; Index < Entries.Num(); ++Index)
    {
        if (Entries[Index].ItemGuid == ItemGuid)
        {
            return &Entries[Index];
        }
    }
    return nullptr;
}

void AFortInventory::HandleInventoryLocalUpdate()
{
    Reflection::FParams Params(StaticFunction_HandleInventoryLocalUpdate());
    Params.Call(this);
}

void AFortInventory::AddWorldItem(UFortWorldItem* WorldItem)
{
#if WITH_SERVER_CODE
    if (!WorldItem)
    {
        return;
    }

    FFortItemEntry& ItemEntry = WorldItem->ItemEntry;
    ItemEntry.ParentInventory = FWeakObjectPtr(this);

    FFortItemList& ItemList = Inventory;
    TStructArray<FFortItemEntry>& Entries = ItemList.ReplicatedEntries;
    FFortItemEntry& ReplicatedEntry = Entries.AddDefaulted();
    FFortItemEntry::StaticStruct()->CopyScriptStruct(&ReplicatedEntry, &ItemEntry);

    TArray<UFortWorldItem*>& Instances = ItemList.ItemInstances;
    Instances.Add(WorldItem);

    ItemList.MarkItemDirty(ReplicatedEntry);
    HandleInventoryLocalUpdate();
#endif
}

void AFortInventory::UpdateWorldItemCount(UFortWorldItem* WorldItem, int32_t NewCount)
{
#if WITH_SERVER_CODE
    if (!WorldItem)
    {
        return;
    }

    FFortItemEntry& ItemEntry = WorldItem->ItemEntry;
    ItemEntry.Count = NewCount;

    FFortItemList& ItemList = Inventory;
    if (FFortItemEntry* ReplicatedEntry = ItemList.FindReplicatedEntry(ItemEntry.ItemGuid))
    {
        ReplicatedEntry->Count = NewCount;
        ItemList.MarkItemDirty(*ReplicatedEntry);
    }
    HandleInventoryLocalUpdate();
#endif
}

void AFortInventory::RemoveWorldItem(UFortWorldItem* WorldItem)
{
#if WITH_SERVER_CODE
    if (!WorldItem)
    {
        return;
    }

    FFortItemList& ItemList = Inventory;
    const FGuid ItemGuid = static_cast<FFortItemEntry&>(WorldItem->ItemEntry).ItemGuid;
    TStructArray<FFortItemEntry>& Entries = ItemList.ReplicatedEntries;
    for (int32_t Index = 0; Index < Entries.Num(); ++Index)
    {
        if (Entries[Index].ItemGuid == ItemGuid)
        {
            Entries.RemoveAt(Index);
            break;
        }
    }

    TArray<UFortWorldItem*>& Instances = ItemList.ItemInstances;
    for (int32_t Index = 0; Index < Instances.Num(); ++Index)
    {
        if (Instances[Index] == WorldItem)
        {
            Instances.RemoveAt(Index);
            break;
        }
    }

    ItemList.MarkArrayDirty();
    HandleInventoryLocalUpdate();
#endif
}
