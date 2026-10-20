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

UFortWorldItem* AFortInventory::FindWorldItem(const FGuid& ItemGuid)
{
    FFortItemList& ItemList = Inventory;
    TArray<UFortWorldItem*>& Instances = ItemList.ItemInstances;
    for (int32_t Index = 0; Index < Instances.Num(); ++Index)
    {
        UFortWorldItem* WorldItem = Instances[Index];
        if (!WorldItem)
        {
            continue;
        }

        FFortItemEntry& ItemEntry = WorldItem->ItemEntry;
        if (ItemEntry.ItemGuid == ItemGuid)
        {
            return WorldItem;
        }
    }
    return nullptr;
}

void AFortInventory::AddWorldItem(UFortWorldItem* WorldItem)
{
#if WITH_SERVER_CODE
    if (!WorldItem)
    {
        return;
    }

    WorldItem->OwnerInventory = this;
    WorldItem->bIsTemporaryItem = false;
    WorldItem->bNeedsPersistentUpdate = true;

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
    WorldItem->bNeedsPersistentUpdate = true;

    FFortItemList& ItemList = Inventory;
    if (FFortItemEntry* ReplicatedEntry = ItemList.FindReplicatedEntry(ItemEntry.ItemGuid))
    {
        ReplicatedEntry->Count = NewCount;
        ItemList.MarkItemDirty(*ReplicatedEntry);
    }
    HandleInventoryLocalUpdate();
#endif
}

void AFortInventory::UpdateWorldItemLoadedAmmo(UFortWorldItem* WorldItem, int32_t NewLoadedAmmo)
{
#if WITH_SERVER_CODE
    if (!WorldItem)
    {
        return;
    }

    FFortItemEntry& ItemEntry = WorldItem->ItemEntry;
    ItemEntry.LoadedAmmo = NewLoadedAmmo;
    WorldItem->bNeedsPersistentUpdate = true;

    FFortItemList& ItemList = Inventory;
    if (FFortItemEntry* ReplicatedEntry = ItemList.FindReplicatedEntry(ItemEntry.ItemGuid))
    {
        ReplicatedEntry->LoadedAmmo = NewLoadedAmmo;
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
