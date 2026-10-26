#include "pch.h"
#include "Items/FortPickup.h"
#include "Curves/CurveBase.h"
#include "Finder.h"
#include "FortAssetManager.h"
#include "FortGameData.h"
#include "Math/UnrealMathUtility.h"
#include "Hooks.h"
#include "Items/FortInventory.h"
#include "Items/FortInventoryOwnerInterface.h"
#include "Items/FortItemDefinition.h"
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
