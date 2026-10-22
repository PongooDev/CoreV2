#include "pch.h"
#include "Items/FortInventory.h"
#include "Finder.h"
#include "Items/FortItemDefinition.h"
#include "Natives.h"
#include "Player/FortPlayerController.h"
#include "Templates/Casts.h"
#include "ReflectionParams.h"
#include "UObject/Class.h"

#include <algorithm>

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

    if (!Natives::AFortInventory_InitializeExistingItem && !Finder::ResolveInventoryLive())
    {
        return;
    }
    Natives::AFortInventory_InitializeExistingItem(this, WorldItem);
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

    WorldItem->SetItemCount(NewCount);
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

    ItemList.MarkArrayDirty();
    HandleInventoryLocalUpdate();
#endif
}

void AFortInventory::RemoveWorldItemCount(UFortWorldItem* WorldItem, int32_t Count)
{
#if WITH_SERVER_CODE
    if (!WorldItem || Count <= 0)
    {
        return;
    }

    FFortItemEntry& ItemEntry = WorldItem->ItemEntry;
    if (Count >= ItemEntry.Count)
    {
        RemoveWorldItem(WorldItem);
    }
    else
    {
        UpdateWorldItemCount(WorldItem, ItemEntry.Count - Count);
    }
#endif
}

int32_t AFortInventory::AddWorldItemCopy(AFortPlayerController* Owner, const FFortItemEntry& Source, int32_t Count, int32_t MaxNewItems, std::vector<UFortWorldItem*>* OutNewItems)
{
#if WITH_SERVER_CODE
    UFortWorldItemDefinition* ItemDefinition = Cast<UFortWorldItemDefinition>(static_cast<UFortItemDefinition*>(Source.ItemDefinition));
    if (!ItemDefinition || Count <= 0)
    {
        return 0;
    }

    const int32_t MaxStackSize = std::max(static_cast<int32_t>(ItemDefinition->MaxStackSize), 1);
    int32_t Remaining = Count;
    FFortItemList& ItemList = Inventory;
    TArray<UFortWorldItem*>& Instances = ItemList.ItemInstances;
    for (int32_t Index = 0; Index < Instances.Num() && Remaining > 0; ++Index)
    {
        UFortWorldItem* Existing = Instances[Index];
        FFortItemEntry* ExistingEntry = Existing ? &static_cast<FFortItemEntry&>(Existing->ItemEntry) : nullptr;
        if (ExistingEntry && static_cast<UFortItemDefinition*>(ExistingEntry->ItemDefinition) == ItemDefinition && ExistingEntry->Count < MaxStackSize)
        {
            const int32_t Merged = std::min(Remaining, MaxStackSize - ExistingEntry->Count);
            UpdateWorldItemCount(Existing, ExistingEntry->Count + Merged);
            Remaining -= Merged;
        }
    }

    for (int32_t NewItems = 0; Remaining > 0 && (MaxNewItems < 0 || NewItems < MaxNewItems); ++NewItems)
    {
        const int32_t StackCount = std::min(Remaining, MaxStackSize);
        UFortWorldItem* WorldItem = Cast<UFortWorldItem>(ItemDefinition->CreateTemporaryItemInstanceBP(StackCount, Source.Level));
        if (!WorldItem)
        {
            break;
        }

        FFortItemEntry& ItemEntry = WorldItem->ItemEntry;
        ItemEntry.Durability = Source.Durability;
        ItemEntry.LoadedAmmo = Source.LoadedAmmo;
        ItemEntry.AlterationDefinitions = Source.AlterationDefinitions;
        if (Owner)
        {
            WorldItem->SetOwningControllerForTemporaryItem(Owner);
        }
        AddWorldItem(WorldItem);
        if (OutNewItems)
        {
            OutNewItems->push_back(WorldItem);
        }
        Remaining -= StackCount;
    }
    return Count - Remaining;
#else
    return 0;
#endif
}
