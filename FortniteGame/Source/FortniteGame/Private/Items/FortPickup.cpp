#include "pch.h"
#include "Items/FortPickup.h"
#include "Finder.h"
#include "Hooks.h"
#include "Items/FortInventory.h"
#include "Items/FortItemDefinition.h"
#include "Log.h"
#include "Missions/FortItemCollectedParams.h"
#include "Natives.h"
#include "Pawns/FortPawn.h"
#include "Player/FortPlayerController.h"
#include "ReflectionParams.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"

#include <vector>

namespace
{
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
    AFortPlayerController* PlayerController = AFortPlayerController::FromInventoryOwner(InventoryOwner);
    if (Role != ROLE_Authority || !PlayerController)
    {
        return;
    }

    std::vector<FFortItemEntry*> Entries{ &static_cast<FFortItemEntry&>(PrimaryPickupItemEntry) };
    TStructArray<FFortItemEntry>& MultiItemEntries = MultiItemPickupEntries;
    for (int32_t Index = 0; Index < MultiItemEntries.Num(); ++Index)
    {
        Entries.push_back(&MultiItemEntries[Index]);
    }

    AFortPawn* Pawn = PlayerController->MyFortPawn;
    UScriptStruct* EntryStruct = FFortItemEntry::StaticStruct();
    for (FFortItemEntry* Entry : Entries)
    {
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
#endif
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
