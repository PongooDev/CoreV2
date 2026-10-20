#include "pch.h"
#include "Items/FortPickup.h"
#include "Finder.h"
#include "Hooks.h"
#include "Items/FortInventory.h"
#include "Items/FortItemDefinition.h"
#include "Log.h"
#include "Missions/FortItemCollectedParams.h"
#include "Natives.h"
#include "Player/FortPlayerController.h"
#include "ReflectionParams.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"

#include <algorithm>

namespace
{
    void GiveItemEntry(AFortPlayerController* PlayerController, const FFortItemEntry& ItemEntry)
    {
        AFortInventory* Inventory = PlayerController->WorldInventory;
        UFortWorldItemDefinition* ItemDefinition = Cast<UFortWorldItemDefinition>(static_cast<UFortItemDefinition*>(ItemEntry.ItemDefinition));
        if (!Inventory || !ItemDefinition || ItemEntry.Count <= 0)
        {
            return;
        }

        const int32_t MaxStackSize = std::max(static_cast<int32_t>(ItemDefinition->MaxStackSize), 1);
        int32_t Remaining = ItemEntry.Count;

        FFortItemList& ItemList = Inventory->Inventory;
        TArray<UFortWorldItem*>& Instances = ItemList.ItemInstances;
        for (int32_t Index = 0; Index < Instances.Num() && Remaining > 0; ++Index)
        {
            UFortWorldItem* Existing = Instances[Index];
            FFortItemEntry& ExistingEntry = Existing->ItemEntry;
            if (static_cast<UFortItemDefinition*>(ExistingEntry.ItemDefinition) == ItemDefinition && ExistingEntry.Count < MaxStackSize)
            {
                const int32_t Added = std::min(Remaining, MaxStackSize - ExistingEntry.Count);
                Inventory->UpdateWorldItemCount(Existing, ExistingEntry.Count + Added);
                Remaining -= Added;
            }
        }

        while (Remaining > 0)
        {
            const int32_t Count = std::min(Remaining, MaxStackSize);
            UFortWorldItem* WorldItem = Cast<UFortWorldItem>(ItemDefinition->CreateTemporaryItemInstanceBP(Count, 1));
            if (!WorldItem)
            {
                return;
            }

            FFortItemEntry& NewEntry = WorldItem->ItemEntry;
            const FGuid ItemGuid = NewEntry.ItemGuid;
            FFortItemEntry::StaticStruct()->CopyScriptStruct(&NewEntry, &ItemEntry);
            NewEntry.ItemGuid = ItemGuid;
            NewEntry.Count = Count;

            WorldItem->SetOwningControllerForTemporaryItem(PlayerController);
            Inventory->AddWorldItem(WorldItem);
            PlayerController->TryAddToQuickBar(WorldItem);
            Remaining -= Count;
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

void AFortPickup::GivePickupTo(IFortInventoryOwnerInterface* InventoryOwner)
{
#if WITH_SERVER_CODE
    AFortPlayerController* PlayerController = AFortPlayerController::FromInventoryOwner(InventoryOwner);
    if (Role != ROLE_Authority || !PlayerController)
    {
        return;
    }

    FFortItemEntry& PrimaryEntry = PrimaryPickupItemEntry;
    GiveItemEntry(PlayerController, PrimaryEntry);
    UFortItemCollectedParams::SendItemCollected(PlayerController, Cast<UFortWorldItemDefinition>(static_cast<UFortItemDefinition*>(PrimaryEntry.ItemDefinition)), PrimaryEntry.Count);
    TStructArray<FFortItemEntry>& MultiItemEntries = MultiItemPickupEntries;
    for (int32_t Index = 0; Index < MultiItemEntries.Num(); ++Index)
    {
        FFortItemEntry& Entry = MultiItemEntries[Index];
        GiveItemEntry(PlayerController, Entry);
        UFortItemCollectedParams::SendItemCollected(PlayerController, Cast<UFortWorldItemDefinition>(static_cast<UFortItemDefinition*>(Entry.ItemDefinition)), Entry.Count);
    }
#endif
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
