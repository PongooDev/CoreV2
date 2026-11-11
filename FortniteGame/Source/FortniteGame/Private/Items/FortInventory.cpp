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

void AFortInventory::ClearShuttingDown()
{
    if (Member_bIsShuttingDown.IsAvailable())
    {
        bIsShuttingDown = false;
    }
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
    // 1.11: InitializeExistingItem only adds an item the inventory doesn't have while bAllowMcpItemCreation is set
    // (0x6BA3D4). It is set when the inventory is made (0x69DD76) and UFortMcpProfileWorld::BindToPlayerController
    // clears it once the profile has loaded (0x760B8A), so in Save the World every item the server made after the
    // player joined was dropped: the harvesting tool, the hero's abilities, the homebase gadgets and new pickups never
    // reached the inventory or the quickbar (#177). The server's own callers are stripped; the item is created with it
    // set, and the inventory goes back to how it was.
    uint8_t* const AllowMcpItemCreation = Natives::AFortInventory_bAllowMcpItemCreation >= 0 ? reinterpret_cast<uint8_t*>(this) + Natives::AFortInventory_bAllowMcpItemCreation : nullptr;
    const uint8_t bAllowMcpItemCreation = AllowMcpItemCreation ? *AllowMcpItemCreation : 1;
    if (AllowMcpItemCreation)
    {
        *AllowMcpItemCreation = 1;
    }
    Natives::AFortInventory_InitializeExistingItem(this, WorldItem);
    if (AllowMcpItemCreation)
    {
        *AllowMcpItemCreation = bAllowMcpItemCreation;
    }
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
            NewEntry.CopyAlterationsFrom(Source);
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
    if (!Natives::AFortInventory_GetMaxStackSize && !Finder::ResolveBackpackLive(AFortPickup::PrimaryPickupItemEntryOffset(), UFortWorldItem::ItemEntryOffset()))
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
    if (!Natives::AFortInventory_GetOverflowAmount && !Finder::ResolveBackpackLive(AFortPickup::PrimaryPickupItemEntryOffset(), UFortWorldItem::ItemEntryOffset()))
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
    if (Natives::AFortInventory_InventoryInterfaceOffset < 0 && !Finder::ResolveBackpackLive(AFortPickup::PrimaryPickupItemEntryOffset(), UFortWorldItem::ItemEntryOffset()))
    {
        return false;
    }

    using FGetItems = int32_t (*)(void* This);
    void* Interface = reinterpret_cast<uint8_t*>(this) + Natives::AFortInventory_InventoryInterfaceOffset;
    void* const* VTable = *reinterpret_cast<void* const* const*>(Interface);
    const int32_t NumItems = reinterpret_cast<FGetItems>(VTable[Natives::IFortInventoryInterface_GetNumItemsIndex])(Interface);
    return NumItems >= reinterpret_cast<FGetItems>(VTable[Natives::IFortInventoryInterface_GetMaxItemsIndex])(Interface);
}

bool AFortInventory::CanStackWith(UFortWorldItem* Item, const FFortItemEntry& ItemEntry, float MaxDurability)
{
    if (Natives::AFortInventory_CanStackWith)
    {
        // 1.8: CanStackWith(UFortWorldItem*, const FFortItemEntry&, float MaxDurability), no inventory argument.
        return Natives::AFortInventory_bControllerLimits
            ? reinterpret_cast<bool (*)(UFortWorldItem*, const FFortItemEntry&, float)>(Natives::AFortInventory_CanStackWith)(Item, ItemEntry, MaxDurability)
            : Natives::AFortInventory_CanStackWith(this, Item, ItemEntry, MaxDurability);
    }
    // 4.5's CanStackWith (0xD7A520), which 5.0 inlines into its stack limit (CL-4204761 0xF142B0): the same item as the
    // stack's, a stack that isn't regenerating, and for an item with durability, a stack at the full durability.
    // Without the build's pieces (the overflow checks weren't found, 5.40 #386) the same item is the same definition at
    // the same level, and the regen cooldown can't be read: an item still stacks instead of never going in.
    const FFortItemEntry& StackEntry = static_cast<FFortItemEntry&>(Item->ItemEntry);
    const bool bPieces = Natives::FFortItemEntry_IsSameItem && Natives::UFortWorldItem_BaseRegenCooldown >= 0;
    const bool bSameItem = bPieces ? Natives::FFortItemEntry_IsSameItem(ItemEntry, StackEntry)
        : static_cast<UFortItemDefinition*>(StackEntry.ItemDefinition) == static_cast<UFortItemDefinition*>(ItemEntry.ItemDefinition) && StackEntry.Level == ItemEntry.Level;
    const float BaseRegenCooldown = bPieces ? *reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(Item) + Natives::UFortWorldItem_BaseRegenCooldown) : 0.0f;
    const UFortWorldItemDefinition* ItemDefinition = Cast<UFortWorldItemDefinition>(static_cast<UFortItemDefinition*>(ItemEntry.ItemDefinition));
    const bool bHasDurability = ItemDefinition && ItemDefinition->ItemHasDurability();
    return bSameItem && !(BaseRegenCooldown > 0.0f) && (!bHasDurability || MaxDurability == StackEntry.Durability);
}

int32_t AFortInventory::AddItemStacks(const FFortItemEntry& ItemEntry, int32_t Count, int32_t MaxNewItems, bool bBackpack, bool bKeepOverflow,
    const std::function<void(UFortWorldItem* Item, int32_t Added)>& TopUp, const std::function<UFortWorldItem*(int32_t StackCount, bool bOverflow)>& NewStack)
{
#if WITH_SERVER_CODE
    UFortWorldItemDefinition* ItemDefinition = Cast<UFortWorldItemDefinition>(static_cast<UFortItemDefinition*>(ItemEntry.ItemDefinition));
    if (!ItemDefinition || Count <= 0)
    {
        return 0;
    }
    // The overflow checks are found on the first add. A build whose checks aren't found still gets its items (5.40's
    // starting items, pickaxe and pickups went nowhere, #386): CanStackWith and GetMaxStackSize fall back to the item
    // definition's own rules, GetOverflowAmount and IsBackpackFull to no backpack limit.
    if (!Natives::AFortInventory_CanStackWith)
    {
        Finder::ResolveBackpackLive(AFortPickup::PrimaryPickupItemEntryOffset(), UFortWorldItem::ItemEntryOffset());
    }

    const int32_t MaxStackSize = GetMaxStackSize(ItemDefinition);
    const float MaxDurability = ItemDefinition->GetMaxDurability(ItemEntry.Level);
    int32_t Remaining = Count;
    bool bHasStack = false;
    const auto CanStack = [&](UFortWorldItem* Existing)
    {
        return CanStackWith(Existing, ItemEntry, MaxDurability);
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
