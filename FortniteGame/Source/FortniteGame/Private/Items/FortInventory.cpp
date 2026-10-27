#include "pch.h"
#include "Items/FortInventory.h"
#include "Finder.h"
#include "Items/FortItemDefinition.h"
#include "Items/FortPickup.h"
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

    const bool bBackpack = Owner && static_cast<AFortInventory*>(Owner->WorldInventory) == this;
    UScriptStruct* EntryStruct = FFortItemEntry::StaticStruct();
    std::vector<uint8_t> EntryStorage(EntryStruct->GetStructureSize(), 0);
    FFortItemEntry* ItemEntry = reinterpret_cast<FFortItemEntry*>(EntryStorage.data());
    EntryStruct->InitializeStruct(ItemEntry);
    EntryStruct->CopyScriptStruct(ItemEntry, &Source);
    ItemEntry->Count = Count;
    if (bBackpack)
    {
        ItemEntry->Count = Count - GetOverflowAmount(*ItemEntry, true);
    }

    const int32_t Added = AddItemStacks(*ItemEntry, ItemEntry->Count, MaxNewItems, bBackpack, false,
        [this](UFortWorldItem* Existing, int32_t StackAdded)
        {
            UpdateWorldItemCount(Existing, static_cast<FFortItemEntry&>(Existing->ItemEntry).Count + StackAdded);
        },
        [&](int32_t StackCount, bool) -> UFortWorldItem*
        {
            UFortWorldItem* WorldItem = Cast<UFortWorldItem>(ItemDefinition->CreateTemporaryItemInstanceBP(StackCount, Source.Level));
            if (!WorldItem)
            {
                return nullptr;
            }

            FFortItemEntry& NewEntry = WorldItem->ItemEntry;
            if (Source.GetIntValue(EFortItemEntryState::GetDurabilityInitialized()) != 0)
            {
                WorldItem->SetDurability(Source.Durability);
            }
            NewEntry.LoadedAmmo = Source.LoadedAmmo;
            NewEntry.AlterationDefinitions = Source.AlterationDefinitions;
            if (Owner)
            {
                WorldItem->SetOwningControllerForTemporaryItem(Owner);
            }
            AddWorldItem(WorldItem);
            if (OutNewItems)
            {
                OutNewItems->push_back(WorldItem);
            }
            return WorldItem;
        });
    EntryStruct->DestroyStruct(ItemEntry);
    return Added;
#else
    return 0;
#endif
}

int32_t AFortInventory::GetMaxStackSize(const UFortItemDefinition* ItemDefinition)
{
    if (!ItemDefinition)
    {
        return 1;
    }
    if (!Natives::AFortInventory_GetMaxStackSize && !Finder::ResolveBackpackLive(AFortPickup::PrimaryPickupItemEntryOffset()))
    {
        return std::max(static_cast<int32_t>(ItemDefinition->MaxStackSize), 1);
    }
    if (Natives::AFortInventory_bControllerLimits)
    {
        // 1.8: GetMaxStackSize(AFortPlayerController*, const UFortItemDefinition*), given the owner the game's own wrappers pass.
        using FGetMaxStackSize = int32_t (*)(AFortPlayerController*, const UFortItemDefinition*);
        return std::max(reinterpret_cast<FGetMaxStackSize>(Natives::AFortInventory_GetMaxStackSize)(Cast<AFortPlayerController>(GetOwner()), ItemDefinition), 1);
    }
    return std::max(Natives::AFortInventory_GetMaxStackSize(this, ItemDefinition), 1);
}

int32_t AFortInventory::GetOverflowAmount(const FFortItemEntry& ItemEntry, bool bForce)
{
#if WITH_SERVER_CODE
    UFortWorldItemDefinition* ItemDefinition = Cast<UFortWorldItemDefinition>(static_cast<UFortItemDefinition*>(ItemEntry.ItemDefinition));
    const int32_t Count = ItemEntry.Count;
    if (!ItemDefinition || Count <= 0 || (!bForce && ItemDefinition->bInventorySizeLimited))
    {
        return 0;
    }
    if (!Natives::AFortInventory_GetOverflowAmount && !Finder::ResolveBackpackLive(AFortPickup::PrimaryPickupItemEntryOffset()))
    {
        return 0;
    }

    return std::clamp(Natives::AFortInventory_GetOverflowAmount(this, ItemEntry), 0, Count);
#else
    return 0;
#endif
}

bool AFortInventory::IsBackpackFull()
{
    if (Natives::AFortInventory_InventoryInterfaceOffset < 0 && !Finder::ResolveBackpackLive(AFortPickup::PrimaryPickupItemEntryOffset()))
    {
        return false;
    }

    using FGetItems = int32_t (*)(void* This);
    void* Interface = reinterpret_cast<uint8_t*>(this) + Natives::AFortInventory_InventoryInterfaceOffset;
    void* const* VTable = *reinterpret_cast<void* const* const*>(Interface);
    const int32_t NumItems = reinterpret_cast<FGetItems>(VTable[Natives::IFortInventoryInterface_GetNumItemsIndex])(Interface);
    return NumItems >= reinterpret_cast<FGetItems>(VTable[Natives::IFortInventoryInterface_GetMaxItemsIndex])(Interface);
}

int32_t AFortInventory::AddItemStacks(const FFortItemEntry& ItemEntry, int32_t Count, int32_t MaxNewItems, bool bBackpack, bool bKeepOverflow,
    const std::function<void(UFortWorldItem* Item, int32_t Added)>& TopUp, const std::function<UFortWorldItem*(int32_t StackCount, bool bOverflow)>& NewStack)
{
#if WITH_SERVER_CODE
    UFortWorldItemDefinition* ItemDefinition = Cast<UFortWorldItemDefinition>(static_cast<UFortItemDefinition*>(ItemEntry.ItemDefinition));
    if (!ItemDefinition || Count <= 0 || (!Natives::AFortInventory_CanStackWith && !Finder::ResolveBackpackLive(AFortPickup::PrimaryPickupItemEntryOffset())))
    {
        return 0;
    }

    const int32_t MaxStackSize = GetMaxStackSize(ItemDefinition);
    const float MaxDurability = ItemDefinition->GetMaxDurability(ItemEntry.Level);
    int32_t Remaining = Count;
    bool bHasStack = false;
    const auto CanStack = [&](UFortWorldItem* Existing)
    {
        // 1.8: CanStackWith(UFortWorldItem*, const FFortItemEntry&, float MaxDurability), no inventory argument.
        return Natives::AFortInventory_bControllerLimits
            ? reinterpret_cast<bool (*)(UFortWorldItem*, const FFortItemEntry&, float)>(Natives::AFortInventory_CanStackWith)(Existing, ItemEntry, MaxDurability)
            : Natives::AFortInventory_CanStackWith(this, Existing, ItemEntry, MaxDurability);
    };

    FFortItemList& ItemList = Inventory;
    TArray<UFortWorldItem*>& Instances = ItemList.ItemInstances;
    for (int32_t Index = 0; Index < Instances.Num(); ++Index)
    {
        UFortWorldItem* Existing = Instances[Index];
        FFortItemEntry* ExistingEntry = Existing ? &static_cast<FFortItemEntry&>(Existing->ItemEntry) : nullptr;
        if (!ExistingEntry || static_cast<UFortItemDefinition*>(ExistingEntry->ItemDefinition) != ItemDefinition || !CanStack(Existing))
        {
            continue;
        }

        bHasStack = true;
        if (Remaining > 0 && ExistingEntry->Count < MaxStackSize)
        {
            const int32_t Added = std::min(Remaining, MaxStackSize - ExistingEntry->Count);
            TopUp(Existing, Added);
            Remaining -= Added;
        }
    }

    const bool bLimited = bBackpack && ItemDefinition->bInventorySizeLimited;
    const bool bAllowMultipleStacks = ItemDefinition->bAllowMultipleStacks;
    for (int32_t NewItems = 0; Remaining > 0 && (MaxNewItems < 0 || NewItems < MaxNewItems) && Instances.Num() < MaxWorldItemStacks && (bAllowMultipleStacks || !bHasStack); ++NewItems)
    {
        const bool bOverflow = bLimited && IsBackpackFull();
        if (bOverflow && !bKeepOverflow)
        {
            break;
        }

        const int32_t StackCount = std::min(Remaining, MaxStackSize);
        if (!NewStack(StackCount, bOverflow))
        {
            break;
        }
        bHasStack = true;
        Remaining -= StackCount;
    }
    return Count - Remaining;
#else
    return 0;
#endif
}
