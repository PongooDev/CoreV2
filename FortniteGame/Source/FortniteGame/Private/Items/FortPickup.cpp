#include "pch.h"
#include "Items/FortPickup.h"
#include "Athena/FortGameModeAthena.h"
#include "Curves/CurveBase.h"
#include "Finder.h"
#include "FortAssetManager.h"
#include "FortGameData.h"
#include "Math/UnrealMathUtility.h"
#include "Hooks.h"
#include "Items/FortInventory.h"
#include "Items/FortInventoryOwnerInterface.h"
#include "Items/FortItemDefinition.h"
#include "Items/FortQuickBars.h"
#include "Log.h"
#include "Missions/FortItemCollectedParams.h"
#include "Natives.h"
#include "Pawns/FortAIPawn.h"
#include "Pawns/FortPawn.h"
#include "Player/FortPlayerController.h"
#include "ReflectionParams.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"
#include "Weapons/FortWeapon.h"

#include <algorithm>
#include <vector>

namespace
{
    std::vector<FFortItemEntry*> GetPickupEntries(AFortPickup* Pickup)
    {
        std::vector<FFortItemEntry*> Entries{ &static_cast<FFortItemEntry&>(Pickup->PrimaryPickupItemEntry) };
        TStructArray<FFortItemEntry>& MultiItemEntries = Pickup->MultiItemPickupEntries;
        for (int32_t Index = 0; Index < MultiItemEntries.Num(); ++Index)
        {
            Entries.push_back(&MultiItemEntries[Index]);
        }
        return Entries;
    }

    int32_t GiveItemEntry(AFortPlayerController* PlayerController, const FFortItemEntry& ItemEntry)
    {
        AFortInventory* Inventory = PlayerController->WorldInventory;
        UFortWorldItemDefinition* ItemDefinition = Cast<UFortWorldItemDefinition>(static_cast<UFortItemDefinition*>(ItemEntry.ItemDefinition));
        if (!Inventory || !ItemDefinition || ItemEntry.Count <= 0)
        {
            return 0;
        }

        const bool bShouldShowItemToast = ItemEntry.ShouldShowItemToast();
        const int32_t Count = ItemEntry.Count - Inventory->GetOverflowAmount(ItemEntry, true);
        return Inventory->AddItemStacks(ItemEntry, Count, -1, true, false,
            [&](UFortWorldItem* Existing, int32_t Added)
            {
                FFortItemEntry& ExistingEntry = Existing->ItemEntry;
                Inventory->UpdateWorldItemCount(Existing, ExistingEntry.Count + Added);
                ExistingEntry.AddNewItemCount(Added, bShouldShowItemToast);
            },
            [&](int32_t StackCount, bool) -> UFortWorldItem*
            {
                UFortWorldItem* WorldItem = Cast<UFortWorldItem>(ItemDefinition->CreateTemporaryItemInstanceBP(StackCount, 1));
                if (!WorldItem)
                {
                    return nullptr;
                }

                FFortItemEntry& NewEntry = WorldItem->ItemEntry;
                const FGuid ItemGuid = NewEntry.ItemGuid;
                FFortItemEntry::StaticStruct()->CopyScriptStruct(&NewEntry, &ItemEntry);
                NewEntry.ItemGuid = ItemGuid;
                NewEntry.Count = StackCount;

                WorldItem->SetOwningControllerForTemporaryItem(PlayerController);
                Inventory->AddWorldItem(WorldItem);
                NewEntry.SetNewItemCount(StackCount, bShouldShowItemToast);
                PlayerController->TryAddToQuickBar(WorldItem);
                return WorldItem;
            });
    }

    // Battle Royale swaps a weapon picked up with a full backpack for the item in the player's hands (#88). Its controller
    // lets the client pick one up then (AFortPlayerControllerAthena's override of the overflow bypass, 1.7.2 0x434C20, slot
    // 0x2B8: a weapon-type item, the backpack's count at its size, and the focused quickbar slot's item, 0x66BF80, one that
    // bCanBeDropped; the pickaxe can't) and shows "Swap equipped" (0x66C3C0), but the swap was in the stripped give: Core's
    // tossed the weapon back down. The held item is dropped as the player's own drop would be; returns the primary slot
    // it emptied, or -1.
    std::vector<FGuid> PrimaryQuickBarItems(AFortQuickBars* Bars)
    {
        std::vector<FGuid> Items;
        const int32_t NumSlots = Bars ? Bars->GetQuickBar(EFortQuickBars::GetPrimary()).Slots.Num() : 0;
        for (int32_t SlotIndex = 0; SlotIndex < NumSlots; ++SlotIndex)
        {
            const TArray<FGuid>& SlotItems = Bars->GetSlot(EFortQuickBars::GetPrimary(), SlotIndex).Items;
            Items.insert(Items.end(), SlotItems.begin(), SlotItems.end());
        }
        return Items;
    }

    // Moves the one primary quickbar item that isn't in Before into Slot, and what was in Slot to where it was.
    void PutNewItemInSlot(AFortQuickBars* Bars, const std::vector<FGuid>& Before, int32_t Slot)
    {
        const EFortQuickBars Primary = EFortQuickBars::GetPrimary();
        const int32_t NumSlots = Bars->GetQuickBar(Primary).Slots.Num();
        int32_t NewSlot = -1;
        FGuid NewItem;
        for (int32_t SlotIndex = 0; SlotIndex < NumSlots && NewSlot < 0; ++SlotIndex)
        {
            for (const FGuid& Item : Bars->GetSlot(Primary, SlotIndex).Items)
            {
                if (std::find(Before.begin(), Before.end(), Item) == Before.end())
                {
                    NewSlot = SlotIndex;
                    NewItem = Item;
                    break;
                }
            }
        }
        if (NewSlot < 0 || NewSlot == Slot || Slot >= NumSlots)
        {
            return;
        }

        const TArray<FGuid>& Displaced = Bars->GetSlot(Primary, Slot).Items;
        const std::vector<FGuid> DisplacedItems(Displaced.begin(), Displaced.end());
        Bars->ServerRemoveItemInternal(NewItem, false, true);
        for (const FGuid& Item : DisplacedItems)
        {
            Bars->ServerRemoveItemInternal(Item, false, true);
        }
        Bars->ServerAddItemInternal(NewItem, Primary, Slot);
        for (const FGuid& Item : DisplacedItems)
        {
            Bars->ServerAddItemInternal(Item, Primary, NewSlot);
        }
    }

    int32_t DropFocusedItemForSwap(AFortPlayerController* PlayerController, const FFortItemEntry& ItemEntry)
    {
        const UFortItemDefinition* ItemDefinition = ItemEntry.ItemDefinition;
        const EFortItemType ItemType = ItemDefinition ? static_cast<EFortItemType>(ItemDefinition->ItemType) : EFortItemType::GetMax_None();
        const bool bWeapon = ItemType == EFortItemType::GetWeapon() || ItemType == EFortItemType::GetWeaponMelee() || ItemType == EFortItemType::GetWeaponRanged();
        AFortInventory* Inventory = PlayerController->WorldInventory;
        AFortQuickBars* Bars = PlayerController->QuickBars;
        if (!bWeapon || !Inventory || !Bars || !AFortGameModeAthena::GetCurrent(PlayerController) || Inventory->GetOverflowAmount(ItemEntry, true) <= 0)
        {
            return -1;
        }

        const EFortQuickBars FocusedBar = Bars->GetCurrentQuickBar();
        const int32_t FocusedSlot = FocusedBar != EFortQuickBars::GetMax_None() ? static_cast<int32_t>(Bars->GetQuickBar(FocusedBar).CurrentFocusedSlot) : -1;
        if (FocusedSlot < 0 || FocusedSlot >= Bars->GetQuickBar(FocusedBar).Slots.Num())
        {
            return -1;
        }
        const TArray<FGuid>& SlotItems = Bars->GetSlot(FocusedBar, FocusedSlot).Items;
        UFortWorldItem* FocusedItem = SlotItems.Num() > 0 ? Inventory->FindWorldItem(SlotItems[0]) : nullptr;
        FFortItemEntry* FocusedEntry = FocusedItem ? &static_cast<FFortItemEntry&>(FocusedItem->ItemEntry) : nullptr;
        UFortWorldItemDefinition* FocusedDefinition = FocusedEntry ? Cast<UFortWorldItemDefinition>(static_cast<UFortItemDefinition*>(FocusedEntry->ItemDefinition)) : nullptr;
        if (!FocusedDefinition || !FocusedDefinition->bCanBeDropped)
        {
            return -1;
        }

        const FGuid FocusedGuid = FocusedEntry->ItemGuid;
        PlayerController->ServerSpawnInventoryDrop_Implementation(FocusedGuid, FocusedEntry->Count);
        if (Inventory->FindWorldItem(FocusedGuid))
        {
            return -1;
        }
        Log::Info(L"Battle Royale: {} swaps {} for {}", PlayerController->GetName().ToStdString(), FocusedDefinition->GetName().ToStdString(), ItemDefinition->GetName().ToStdString());
        return FocusedBar == EFortQuickBars::GetPrimary() ? FocusedSlot : -1;
    }
}

void AFortPickup::CombineItemEntries(TStructArray<FFortItemEntry>& Entries, AFortInventory* Inventory)
{
    // UFortLootPackage::PickLootDrops (as the game's) only merges items that can't take more than one stack (resources,
    // ammo), so every roll of an ingredient such as planks stays its own entry. The pickups' own combining after the toss
    // is stripped (its timer is bound to the shared empty function), so Core merges before spawning. Weapons and other
    // single items keep a stack size of 1 and stay apart.
    if (!Inventory)
    {
        return;
    }
    for (int32_t Index = 0; Index < Entries.Num(); ++Index)
    {
        FFortItemEntry& Entry = Entries[Index];
        const UFortItemDefinition* Definition = Entry.ItemDefinition;
        const int32_t MaxStackSize = Definition ? std::max(Inventory->GetMaxStackSize(Definition), 1) : 1;
        for (int32_t Other = Index + 1; Other < Entries.Num() && Entry.Count < MaxStackSize; ++Other)
        {
            FFortItemEntry& Candidate = Entries[Other];
            if (static_cast<UFortItemDefinition*>(Candidate.ItemDefinition) != Definition || Candidate.Level != Entry.Level)
            {
                continue;
            }
            const int32_t Moved = std::min<int32_t>(Candidate.Count, MaxStackSize - Entry.Count);
            Entry.Count = Entry.Count + Moved;
            Candidate.Count = Candidate.Count - Moved;
            if (Candidate.Count <= 0)
            {
                Entries.RemoveAt(Other--);
            }
        }
    }
}

AFortPickup* AFortPickup::CreatePickup(UWorld* World, const FFortItemEntry& ItemEntry, const FVector* SpawnLocation, const FRotator* SpawnRotation, AFortPlayerController* OptionalOwner, UClass* OverrideClass)
{
    if (!Natives::AFortPickup_CreatePickup && !Finder::ResolvePickupLive())
    {
        return nullptr;
    }
    return Natives::AFortPickup_CreatePickup(World, ItemEntry, SpawnLocation, SpawnRotation, OptionalOwner, OverrideClass);
}

void AFortPickup::TossPickup(const FVector& FinalLocation, AFortPawn* ItemOwner, int32_t OverrideMaxStackCount)
{
    Reflection::FParams Params(StaticFunction_TossPickup());
    Params.Set(L"FinalLocation", FinalLocation);
    Params.Set(L"ItemOwner", ItemOwner);
    Params.Set(L"OverrideMaxStackCount", OverrideMaxStackCount);
    Params.Call(this);
}

void AFortPickup::SetPickupTarget(AFortPawn* PickupTarget, float InFlyTime, const FVector& InStartDirection, bool bPlayPickupSound)
{
    if (!Natives::AFortPickup_SetPickupTarget && !Finder::ResolvePickupGiveLive())
    {
        return;
    }
    Natives::AFortPickup_SetPickupTarget(this, PickupTarget, InFlyTime, InStartDirection, bPlayPickupSound);
}

void AFortPickup::SetOptionalMissionGuid(const FGuid& InMissionGuid)
{
    if (Role == ROLE_Authority)
    {
        OptionalMissionGuid = InMissionGuid;
    }
}

void AFortPickup::GivePickupTo(IFortInventoryOwnerInterface* InventoryOwner)
{
#if WITH_SERVER_CODE
    UObject* OwnerObject = InventoryOwner ? InventoryOwner->_getUObject() : nullptr;
    if (Role != ROLE_Authority || !OwnerObject)
    {
        return;
    }
    if (AFortAIPawn* AIPawn = Cast<AFortAIPawn>(OwnerObject))
    {
        GivePickupToAI(AIPawn, InventoryOwner);
        return;
    }
    AFortPlayerController* PlayerController = Cast<AFortPlayerController>(OwnerObject);
    if (!PlayerController)
    {
        return;
    }

    std::vector<FFortItemEntry*> Entries = GetPickupEntries(this);
    AFortPawn* Pawn = PlayerController->MyFortPawn;
    UScriptStruct* EntryStruct = FFortItemEntry::StaticStruct();
    int32_t SwappedSlot = -1;
    std::vector<FGuid> QuickBarItemsBeforeSwap;
    for (FFortItemEntry* Entry : Entries)
    {
        if (SwappedSlot < 0)
        {
            SwappedSlot = DropFocusedItemForSwap(PlayerController, *Entry);
            QuickBarItemsBeforeSwap = SwappedSlot >= 0 ? PrimaryQuickBarItems(PlayerController->QuickBars) : std::vector<FGuid>();
        }
        const int32_t Given = GiveItemEntry(PlayerController, *Entry);
        if (Given > 0)
        {
            UFortItemCollectedParams::SendItemCollected(PlayerController, Cast<UFortWorldItemDefinition>(static_cast<UFortItemDefinition*>(Entry->ItemDefinition)), Given);
        }
        if (Given >= Entry->Count || !Pawn)
        {
            continue;
        }

        std::vector<uint8_t> EntryStorage(EntryStruct->GetStructureSize(), 0);
        FFortItemEntry* Leftover = reinterpret_cast<FFortItemEntry*>(EntryStorage.data());
        EntryStruct->InitializeStruct(Leftover);
        EntryStruct->CopyScriptStruct(Leftover, Entry);
        Leftover->Count = Entry->Count - Given;

        const FVector Location = Pawn->GetTransform().GetLocation();
        if (AFortPickup* Pickup = CreatePickup(GetWorld(), *Leftover, &Location, nullptr, OptionalOwnerID != -1 ? PlayerController : nullptr))
        {
            Pickup->SetOptionalMissionGuid(OptionalMissionGuid);
            Pickup->TossPickup(Location, Pawn, 0);
        }
        EntryStruct->DestroyStruct(Leftover);
    }

    // The new weapon goes into the emptied slot and the player's hands. TryAddToQuickBar puts it in the first free slot
    // it finds, which wasn't always the emptied one (#88: it landed one slot along), so it's moved there, and whatever
    // took the emptied slot gets the new weapon's place.
    AFortQuickBars* Bars = PlayerController->QuickBars;
    if (SwappedSlot >= 0 && Bars)
    {
        PutNewItemInSlot(Bars, QuickBarItemsBeforeSwap, SwappedSlot);
        if (Bars->GetSlot(EFortQuickBars::GetPrimary(), SwappedSlot).Items.Num() > 0)
        {
            Bars->ServerActivateSlotInternal(EFortQuickBars::GetPrimary(), SwappedSlot, 0.0f, true);
        }
    }
#endif
}

void AFortPickup::GivePickupToAI(AFortAIPawn* AIPawn, IFortInventoryOwnerInterface* InventoryOwner)
{
#if WITH_SERVER_CODE
    AFortInventory* Inventory = Natives::IFortInventoryOwnerInterface_GetWorldInventoryIndex >= 0 ? InventoryOwner->GetWorldInventory() : nullptr;
    if (!Inventory)
    {
        return;
    }

    AFortWeapon* OldWeapon = AIPawn->CurrentWeapon;
    const FVector Location = AIPawn->GetTransform().GetLocation();
    UScriptStruct* EntryStruct = FFortItemEntry::StaticStruct();
    std::vector<uint8_t> EntryStorage(EntryStruct->GetStructureSize(), 0);
    FFortItemEntry* DropEntry = reinterpret_cast<FFortItemEntry*>(EntryStorage.data());
    const auto TossEntry = [&](const FFortItemEntry& Entry, int32_t Count)
    {
        EntryStruct->InitializeStruct(DropEntry);
        EntryStruct->CopyScriptStruct(DropEntry, &Entry);
        DropEntry->Count = Count;
        if (AFortPickup* Pickup = CreatePickup(GetWorld(), *DropEntry, &Location))
        {
            Pickup->SetOptionalMissionGuid(OptionalMissionGuid);
            Pickup->TossPickup(Location, AIPawn, 0);
        }
        EntryStruct->DestroyStruct(DropEntry);
    };

    UFortWeaponItemDefinition* WeaponDefinition = nullptr;
    UFortWorldItem* WeaponItem = nullptr;
    for (FFortItemEntry* Entry : GetPickupEntries(this))
    {
        std::vector<UFortWorldItem*> NewItems;
        const int32_t Given = Inventory->AddWorldItemCopy(nullptr, *Entry, Entry->Count, -1, &NewItems);
        if (Given < Entry->Count)
        {
            TossEntry(*Entry, Entry->Count - Given);
        }
        UFortWeaponItemDefinition* Definition = Cast<UFortWeaponItemDefinition>(static_cast<UFortItemDefinition*>(Entry->ItemDefinition));
        if (!WeaponItem && Definition && !NewItems.empty())
        {
            WeaponDefinition = Definition;
            WeaponItem = NewItems.front();
        }
    }
    if (!WeaponItem)
    {
        return;
    }

    const FGuid OldGuid = OldWeapon ? static_cast<FGuid>(OldWeapon->ItemEntryGuid) : FGuid();
    AFortWeapon* NewWeapon = AIPawn->EquipWeaponDefinition(WeaponDefinition, static_cast<FFortItemEntry&>(WeaponItem->ItemEntry).ItemGuid);
    if (!NewWeapon || !OldWeapon || NewWeapon == OldWeapon)
    {
        return;
    }
    AIPawn->RemoveAbilityWeapon(OldWeapon);

    UFortWorldItem* OldItem = OldGuid.IsValid() ? Inventory->FindWorldItem(OldGuid) : nullptr;
    if (!OldItem || OldItem == WeaponItem)
    {
        return;
    }

    FFortItemEntry& OldEntry = OldItem->ItemEntry;
    TossEntry(OldEntry, OldEntry.Count);
    Inventory->RemoveWorldItem(OldItem);
#endif
}

float AFortPickup::GetRandomFlyTime()
{
    UFortGameData* GameData = UFortAssetManager::Get().GetGameData();
    UCurveVector* SplineCurve = GameData ? static_cast<UCurveVector*>(GameData->PickupSplineCurve) : nullptr;
    if (!SplineCurve)
    {
        return 1.5f;
    }
    float MinTime = 0.0f;
    float MaxTime = 0.0f;
    SplineCurve->GetTimeRange(MinTime, MaxTime);
    const float RandomMax = GameData->PickupSplineRandomMax;
    return (MaxTime - MinTime) * ((FMath::FRand() * (RandomMax - 1.0f)) + 1.0f);
}

ptrdiff_t AFortPickup::PrimaryPickupItemEntryOffset()
{
    return Member_PrimaryPickupItemEntry.GetOffsetChecked();
}

void AFortPickup::HookVTables()
{
    if (Natives::AFortPickup_GivePickupToIndex < 0 && !Finder::ResolvePickupGiveLive())
    {
        return;
    }

    const int32_t GiveIndex = Natives::AFortPickup_GivePickupToIndex;
    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Class = *It;
        UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
        void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
        if (VTable && VTable[GiveIndex] == Natives::SharedEmptyFunction)
        {
            Hooks::SwapVTable(L"AFortPickup::GivePickupTo", VTable, GiveIndex, reinterpret_cast<void*>(&GivePickupToHK));
        }
    }
}

void AFortPickup::GivePickupToHK(AFortPickup* This, IFortInventoryOwnerInterface* InventoryOwner)
{
    This->GivePickupTo(InventoryOwner);
}
