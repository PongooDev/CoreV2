#include "pch.h"
#include "Items/FortPickup.h"
#include "Athena/FortGameModeAthena.h"
#include "Curves/CurveBase.h"
#include "Finder.h"
#include "FortAssetManager.h"
#include "FortGameData.h"
#include "GameFramework/ProjectileMovementComponent.h"
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
#include <cmath>
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

    // A weapon swapped in for the dropped one goes into the primary slot the dropped one left (#88). The quickbar's add
    // (1.7.2 0x67E220) keeps one item per slot and overwrites the one there (0x65F580); when the slot it is asked for is
    // disabled or doesn't take the item's type it moves on to the next slot that does, occupied or not. That is how the
    // new weapon took the next slot and pushed that slot's item off the quickbar (the report's screenshot: an empty slot
    // with no key, the backpack at 5 of 5 with 4 items shown), and moving it back afterwards went through the same add.
    // The slot is cleared of entries for items no longer in the inventory and enabled before the add, and never
    // overwritten; returns whether the item is in it.
    bool PutInSwappedSlot(AFortPlayerController* PlayerController, UFortWorldItem* Item, int32_t Slot)
    {
        AFortQuickBars* Bars = PlayerController->QuickBars;
        AFortInventory* Inventory = PlayerController->WorldInventory;
        const EFortQuickBars Primary = EFortQuickBars::GetPrimary();
        FFortItemEntry& ItemEntry = Item->ItemEntry;
        const UFortItemDefinition* ItemDefinition = ItemEntry.ItemDefinition;
        if (!Bars || !Inventory || !ItemDefinition || Slot <= 0 || Slot >= Bars->GetQuickBar(Primary).Slots.Num() || !Bars->GetSlotData(Primary, Slot).AcceptedItemTypes.Contains(ItemDefinition->ItemType))
        {
            return false;
        }

        FQuickBarSlot& SwapSlot = Bars->GetSlot(Primary, Slot);
        TArray<FGuid>& SlotItems = SwapSlot.Items;
        int32_t StaleItems = 0;
        for (int32_t Index = SlotItems.Num() - 1; Index >= 0; --Index)
        {
            if (!Inventory->FindWorldItem(SlotItems[Index]))
            {
                SlotItems.RemoveAt(Index);
                ++StaleItems;
            }
        }
        if (SlotItems.Num() > 0)
        {
            Log::Warning(L"Battle Royale: {}'s quickbar slot {} still holds another item, the swapped-in weapon goes to the first free slot", PlayerController->GetName().ToStdString(), Slot);
            return false;
        }

        const bool bWasEnabled = SwapSlot.bEnabled;
        if (!bWasEnabled)
        {
            Bars->EnableSlot(Primary, Slot);
        }
        if (!bWasEnabled || StaleItems > 0)
        {
            Log::Warning(L"Battle Royale: {}'s quickbar slot {} was {} with {} stale entries", PlayerController->GetName().ToStdString(), Slot, bWasEnabled ? L"enabled" : L"disabled", StaleItems);
        }

        const FGuid ItemGuid = ItemEntry.ItemGuid;
        Bars->ServerAddItemInternal(ItemGuid, Primary, Slot);
        if (Bars->GetSlot(Primary, Slot).Items.Contains(ItemGuid))
        {
            Log::Info(L"Battle Royale: {}'s swapped-in {} is in quickbar slot {}", PlayerController->GetName().ToStdString(), ItemDefinition->GetName().ToStdString(), Slot);
            return true;
        }
        Log::Warning(L"Battle Royale: the quickbar didn't put {}'s swapped-in weapon in slot {}", PlayerController->GetName().ToStdString(), Slot);
        return false;
    }

    int32_t GiveItemEntry(AFortPlayerController* PlayerController, const FFortItemEntry& ItemEntry, int32_t SwappedSlot = -1)
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
                if (SwappedSlot < 0 || !PutInSwappedSlot(PlayerController, WorldItem, SwappedSlot))
                {
                    PlayerController->TryAddToQuickBar(WorldItem);
                }
                SwappedSlot = -1;
                return WorldItem;
            });
    }

    // Battle Royale swaps a weapon picked up with a full backpack for the item in the player's hands (#88). Its controller
    // lets the client pick one up then (AFortPlayerControllerAthena's override of the overflow bypass, 1.7.2 0x434C20, slot
    // 0x2B8: a weapon-type item, the backpack's count at its size, and the focused quickbar slot's item, 0x66BF80, one that
    // bCanBeDropped; the pickaxe can't) and shows "Swap equipped" (0x66C3C0), but the swap was in the stripped give: Core's
    // tossed the weapon back down. The held item is dropped as the player's own drop would be; returns the primary slot
    // it emptied, or -1.
    //
    // The slot stays focused through the drop. Removing the focused item activates the previous slot, or the harvesting
    // tool's (slot 0), and that activation sends ClientExecuteInventoryItem for its item (1.7.2 0x67E190 -> 0x67F240 ->
    // 0x8058A0), which the client equips and focuses on its own quickbar (0x7F8E30 -> 0x683DF0). The new weapon's
    // activation that follows reaches the client before the inventory and quickbar that hold it (an RPC goes out at
    // once, the actors replicate at the end of the tick), so the client keeps it pending and runs it again on the next
    // inventory update (0x82EAB0), when its own quickbar may still lack the weapon: it then can't focus the weapon's
    // slot, and the server's focus never changed, so nothing corrects it. The player held the new weapon with the
    // pickaxe slot, or the previous one, focused (#88).
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

        // Only a primary slot that takes the new weapon is swapped, never the harvesting tool's.
        const EFortQuickBars Primary = EFortQuickBars::GetPrimary();
        const int32_t FocusedSlot = Bars->GetCurrentQuickBar() == Primary ? static_cast<int32_t>(Bars->GetQuickBar(Primary).CurrentFocusedSlot) : -1;
        if (FocusedSlot <= 0 || FocusedSlot >= Bars->GetQuickBar(Primary).Slots.Num() || !Bars->GetSlotData(Primary, FocusedSlot).AcceptedItemTypes.Contains(ItemType))
        {
            return -1;
        }
        const TArray<FGuid>& SlotItems = Bars->GetSlot(Primary, FocusedSlot).Items;
        UFortWorldItem* FocusedItem = SlotItems.Num() > 0 ? Inventory->FindWorldItem(SlotItems[0]) : nullptr;
        FFortItemEntry* FocusedEntry = FocusedItem ? &static_cast<FFortItemEntry&>(FocusedItem->ItemEntry) : nullptr;
        UFortWorldItemDefinition* FocusedDefinition = FocusedEntry ? Cast<UFortWorldItemDefinition>(static_cast<UFortItemDefinition*>(FocusedEntry->ItemDefinition)) : nullptr;
        if (!FocusedDefinition || !FocusedDefinition->bCanBeDropped)
        {
            return -1;
        }

        const FGuid FocusedGuid = FocusedEntry->ItemGuid;
        {
            AFortQuickBars::FKeepFocusOnRemove KeepFocus;
            PlayerController->ServerSpawnInventoryDrop_Implementation(FocusedGuid, FocusedEntry->Count);
        }
        if (Inventory->FindWorldItem(FocusedGuid))
        {
            return -1;
        }
        Log::Info(L"Battle Royale: {} swaps {} for {} in quickbar slot {} (slot 0 is the harvesting tool's)", PlayerController->GetName().ToStdString(), FocusedDefinition->GetName().ToStdString(), ItemDefinition->GetName().ToStdString(), FocusedSlot);
        return FocusedSlot;
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

void AFortPickup::TossFromContainer(const FVector& FloorLocation)
{
    // A toss (1.7.2 0x6622F0, run by 0x680570 on the server) launches the pickup's projectile movement, and clients play
    // it from the replicated movement (the pickup's PostNetReceiveVelocity, 0x67BAD0, feeds the projectile). A
    // container's loot is launched onto its spot like a dropped item's (TossOntoFloor), and bTossedFromContainer tells
    // clients (OnRep_TossedFromContainer, 0x67A830: the pickup effect's OnTossed and the touch collision).
    TossOntoFloor(FloorLocation, nullptr);
    if (Member_bTossedFromContainer.IsAvailable())
    {
        bTossedFromContainer = true;
    }
}

void AFortPickup::TossOntoFloor(const FVector& FloorLocation, AFortPawn* ItemOwner)
{
    // A toss doesn't reach the location it is given (#89). 0x680570 launches at 45 degrees with a horizontal speed of the
    // distance over the fly time (about 1.4s) and drops the gravity its solver (0x2A7160) works out for that flight, so the
    // pickup falls under the world's (2800 in Battle Royale): from a body's height, aimed 1.5 to 2.5m out, it came down
    // 10 to 35 out, and the items lay in a pile. The launch is replaced with one solved for the movement's own gravity: up
    // Rise over the higher of the start and the landing, down onto the floor with the pickup's collision resting on it
    // (its root is 80 above it), with the horizontal speed spread over the flight and the hops that follow (each bounce
    // keeps Bounciness of the vertical speed and 1 - Friction of the horizontal: 0.6 and 0.5). Measured on a 1.7.2 server,
    // pickups come to rest within 10 of their spot, after about a second. One aimed past a ledge still lands on the upper
    // floor, short of it.
    constexpr float Rise = 70.0f;
    TossPickup(FloorLocation, ItemOwner, 0);
    UProjectileMovementComponent* Movement = MovementComponent;
    const float Gravity = Movement && Role == ROLE_Authority ? -Movement->GetGravityZ() : 0.0f;
    if (Gravity <= 0.0f)
    {
        return;
    }

    const FVector Start = GetTransform().GetLocation();
    FVector Origin;
    FVector Extent;
    GetActorBounds(true, Origin, Extent);
    const float RestHeight = FMath::Max(Start.Z - (Origin.Z - Extent.Z), 0.0f);
    const FVector Target(FloorLocation.X, FloorLocation.Y, FloorLocation.Z + RestHeight);
    const float Apex = std::max(Start.Z, Target.Z) + Rise;
    const float UpSpeed = std::sqrt(2.0f * Gravity * (Apex - Start.Z));
    const float FallSpeed = std::sqrt(2.0f * Gravity * (Apex - Target.Z));
    const float Restitution = FMath::Clamp(Movement->Bounciness * (1.0f - Movement->Friction), 0.0f, 0.9f);
    const float HopTime = 2.0f * FallSpeed / Gravity * Restitution / (1.0f - Restitution);
    const float FlyTime = (UpSpeed + FallSpeed) / Gravity;
    const float Distance = std::sqrt(FMath::Square(Target.X - Start.X) + FMath::Square(Target.Y - Start.Y));
    const float Speed = Distance / (FlyTime + HopTime);
    const FVector Direction = Distance > 1.0f ? FVector((Target.X - Start.X) / Distance, (Target.Y - Start.Y) / Distance, 0.0f) : FVector(0.0f, 0.0f, 0.0f);
    const FVector LaunchVelocity(Direction.X * Speed, Direction.Y * Speed, UpSpeed);
    Movement->Velocity = LaunchVelocity;
    Movement->MaxSpeed = LaunchVelocity.Size();
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
    for (FFortItemEntry* Entry : Entries)
    {
        int32_t EntrySwappedSlot = -1;
        if (SwappedSlot < 0)
        {
            SwappedSlot = EntrySwappedSlot = DropFocusedItemForSwap(PlayerController, *Entry);
        }
        const int32_t Given = GiveItemEntry(PlayerController, *Entry, EntrySwappedSlot);
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

    // The new weapon, in the emptied slot (PutInSwappedSlot), goes into the player's hands; the slot kept the focus. If
    // it went elsewhere, the focus moves on as the drop would have moved it.
    AFortQuickBars* Bars = PlayerController->QuickBars;
    if (SwappedSlot >= 0 && Bars)
    {
        const EFortQuickBars Primary = EFortQuickBars::GetPrimary();
        if (Bars->GetSlot(Primary, SwappedSlot).Items.Num() > 0)
        {
            Bars->ServerActivateSlotInternal(Primary, SwappedSlot, 0.0f, false);
        }
        else
        {
            Log::Warning(L"Battle Royale: {}'s quickbar slot {} is empty after the swap", PlayerController->GetName().ToStdString(), SwappedSlot);
            Bars->ActivateReplacementSlot();
        }
        FQuickBar& PrimaryQuickBar = Bars->GetQuickBar(Primary);
        Log::Info(L"Battle Royale: after {}'s swap, quickbar slot {} is focused (previously {})", PlayerController->GetName().ToStdString(), static_cast<int32_t>(PrimaryQuickBar.CurrentFocusedSlot), static_cast<int32_t>(PrimaryQuickBar.PreviousFocusedSlot));
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
