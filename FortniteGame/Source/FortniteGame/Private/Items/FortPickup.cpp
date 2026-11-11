#include "pch.h"
#include "Items/FortPickup.h"
#include "Athena/FortGameModeAthena.h"
#include "Components/SceneComponent.h"
#include "Curves/CurveBase.h"
#include "Engine/EngineTypes.h"
#include "Engine/World.h"
#include "Finder.h"
#include "FortAssetManager.h"
#include "FortGameData.h"
#include "FortGameState.h"
#include "FortWorldManager.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "GameThread.h"
#include "Math/UnrealMathUtility.h"
#include "Hooks.h"
#include "Items/FortInventory.h"
#include "Items/FortInventoryOwnerInterface.h"
#include "Items/FortItemDefinition.h"
#include "Items/FortQuickBars.h"
#include "Log.h"
#include "Missions/FortItemCollectedParams.h"
#include "Natives.h"
#include "ObjectLayout.h"
#include "Pawns/FortAIPawn.h"
#include "Pawns/FortPawn.h"
#include "Pawns/FortPlayerPawn.h"
#include "Player/FortPlayerController.h"
#include "ReflectionParams.h"
#include "Templates/Casts.h"
#include "TimerManager.h"
#include "UObject/Class.h"
#include "UObject/ScriptDelegates.h"
#include "UObject/Stack.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"
#include "UObject/WeakObjectPtr.h"
#include "Weapons/FortWeapon.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <utility>
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

    // Battle Royale swaps a weapon (or consumable) picked up with a full backpack for the item in the player's hands (#88,
    // #124). Its controller lets the client pick one up then (AFortPlayerControllerAthena's override of the overflow
    // bypass, 1.7.2 0x434C20, slot 0x2B8, which the pickup's CanInteract 0x6618E0 asks before the overflow: a swappable
    // item type, the backpack's count at its size, and the focused quickbar slot's item, 0x66BF80, one that
    // bCanBeDropped; the pickaxe can't) and shows "Swap equipped" (0x66C3C0), but the swap was in the stripped give: Core's
    // tossed the weapon back down. The held item is dropped as the player's own drop would be; returns the primary slot
    // it emptied, or -1, and the dropped item's definition in OutDroppedDefinition.
    //
    // The slot stays focused through the drop. Removing the focused item activates the previous slot, or the harvesting
    // tool's (slot 0), and that activation sends ClientExecuteInventoryItem for its item (1.7.2 0x67E190 -> 0x67F240 ->
    // 0x8058A0), which the client equips and focuses on its own quickbar (0x7F8E30 -> 0x683DF0). The new weapon's
    // activation that follows reaches the client before the inventory and quickbar that hold it (an RPC goes out at
    // once, the actors replicate at the end of the tick), so the client keeps it pending and runs it again on the next
    // inventory update (0x82EAB0), when its own quickbar may still lack the weapon: it then can't focus the weapon's
    // slot, and the server's focus never changed, so nothing corrects it. The player held the new weapon with the
    // pickaxe slot, or the previous one, focused (#88).
    //
    // The item types are the override's own. 1.1 to 1.8 (1.7.2 0x434C20, 1.8 0x438A30) take Weapon, WeaponMelee and
    // WeaponRanged, the type consumables had too. 1.8.1 added EFortItemType::Consumable (enum value 6, the weapon types
    // moved up one) and gave it to the consumables (Athena_Shields, Bandage, Medkit...: ItemType Consumable), and its
    // override (0x427BD0, AFortPlayerControllerAthena slot 0x2BC) takes Consumable as well (#124). Core's swap only
    // took the weapon types, so on 1.8.1 a consumable picked up with a full backpack flew in and was tossed straight
    // back down, the held item staying where it was.
    //
    // A stack that part fits, the overflow short of the pickup's count, isn't swapped: what fits tops up the player's
    // stacks and the rest goes back down, as before. Only an item with no room at all takes the held slot, so ammo,
    // resources and a consumable that stacks onto the player's own still stack.
    bool IsSwappableItemType(EFortItemType ItemType)
    {
        const EFortItemType Consumable = EFortItemType::GetConsumable();
        return ItemType == EFortItemType::GetWeapon() || ItemType == EFortItemType::GetWeaponMelee() || ItemType == EFortItemType::GetWeaponRanged()
            || (Consumable != Reflection::InvalidEnumValue && ItemType == Consumable);
    }

    int32_t DropFocusedItemForSwap(AFortPlayerController* PlayerController, const FFortItemEntry& ItemEntry, const UFortItemDefinition*& OutDroppedDefinition)
    {
        const UFortItemDefinition* ItemDefinition = ItemEntry.ItemDefinition;
        const EFortItemType ItemType = ItemDefinition ? static_cast<EFortItemType>(ItemDefinition->ItemType) : EFortItemType::GetMax_None();
        AFortInventory* Inventory = PlayerController->WorldInventory;
        AFortQuickBars* Bars = PlayerController->QuickBars;
        if (!ItemDefinition || !Inventory || !Bars || ItemEntry.Count <= 0 || !AFortGameModeAthena::GetCurrent(PlayerController))
        {
            return -1;
        }
        const int32_t Overflow = IsSwappableItemType(ItemType) ? Inventory->GetOverflowAmount(ItemEntry, true) : 0;
        if (Overflow < ItemEntry.Count)
        {
            if (Overflow > 0)
            {
                Log::Info(L"Battle Royale: {} of {}'s {} {} fit, the rest goes back down instead of a swap", static_cast<int32_t>(ItemEntry.Count) - Overflow,PlayerController->GetName().ToStdString(), static_cast<int32_t>(ItemEntry.Count), ItemDefinition->GetName().ToStdString());
            }
            return -1;
        }

        // Only a primary slot that takes the new item is swapped, never the harvesting tool's.
        const EFortQuickBars Primary = EFortQuickBars::GetPrimary();
        int32_t FocusedSlot = Bars->GetCurrentQuickBar() == Primary ? static_cast<int32_t>(Bars->GetQuickBar(Primary).CurrentFocusedSlot) : -1;
        // On 3.5 (CL-4000805) the client has its own quickbar and never tells the server it switched slots
        // (AFortQuickBars::IsReplicatedToOwner). The server's CurrentFocusedSlot follows the client's executes now, but it
        // stayed at the harvesting tool's 0 (a full backpack never swapped: every pickup flew in and back down) or at the
        // slot the server last focused (#269: the swap took that slot, not the held weapon's). The slot is the one holding
        // the pawn's current weapon's item: a weapon in none of the primary slots past the tool's (the harvesting tool
        // itself) takes no slot, so it isn't swapped.
        const AFortPawn* Pawn = PlayerController->MyFortPawn;
        const AFortWeapon* Weapon = Pawn ? static_cast<AFortWeapon*>(Pawn->CurrentWeapon) : nullptr;
        if (FocusedSlot >= 0 && Weapon)
        {
            FocusedSlot = 0;
            const int32_t SlotCount = Bars->GetQuickBar(Primary).Slots.Num();
            for (int32_t Slot = 1; Slot < SlotCount; ++Slot)
            {
                const TArray<FGuid>& Items = Bars->GetSlot(Primary, Slot).Items;
                if (Items.Num() > 0 && Items[0] == Weapon->ItemEntryGuid)
                {
                    FocusedSlot = Slot;
                    break;
                }
            }
        }
        if (FocusedSlot <= 0 || FocusedSlot >= Bars->GetQuickBar(Primary).Slots.Num() || !Bars->GetSlotData(Primary, FocusedSlot).AcceptedItemTypes.Contains(ItemType))
        {
            Log::Info(L"Battle Royale: {}'s {} isn't swapped: the focused slot is {} on the {} bar", PlayerController->GetName().ToStdString(), ItemDefinition->GetName().ToStdString(), FocusedSlot,
                Bars->GetCurrentQuickBar() == Primary ? L"primary" : L"other");
            return -1;
        }
        const TArray<FGuid>& SlotItems = Bars->GetSlot(Primary, FocusedSlot).Items;
        UFortWorldItem* FocusedItem = SlotItems.Num() > 0 ? Inventory->FindWorldItem(SlotItems[0]) : nullptr;
        FFortItemEntry* FocusedEntry = FocusedItem ? &static_cast<FFortItemEntry&>(FocusedItem->ItemEntry) : nullptr;
        UFortWorldItemDefinition* FocusedDefinition = FocusedEntry ? Cast<UFortWorldItemDefinition>(static_cast<UFortItemDefinition*>(FocusedEntry->ItemDefinition)) : nullptr;
        if (!FocusedDefinition || !FocusedDefinition->bCanBeDropped)
        {
            Log::Info(L"Battle Royale: {}'s {} isn't swapped: quickbar slot {} holds {}", PlayerController->GetName().ToStdString(), ItemDefinition->GetName().ToStdString(), FocusedSlot,
                FocusedDefinition ? L"an item that can't be dropped" : L"no item");
            return -1;
        }

        const FGuid FocusedGuid = FocusedEntry->ItemGuid;
        {
            AFortQuickBars::FKeepFocusOnRemove KeepFocus;
            PlayerController->ServerSpawnInventoryDrop_Implementation(FocusedGuid, FocusedEntry->Count);
        }
        if (Inventory->FindWorldItem(FocusedGuid))
        {
            Log::Warning(L"Battle Royale: {}'s {} in quickbar slot {} wasn't dropped, so {} isn't swapped in", PlayerController->GetName().ToStdString(), FocusedDefinition->GetName().ToStdString(), FocusedSlot, ItemDefinition->GetName().ToStdString());
            return -1;
        }
        Log::Info(L"Battle Royale: {} swaps {} for {} in quickbar slot {} (slot 0 is the harvesting tool's)", PlayerController->GetName().ToStdString(), FocusedDefinition->GetName().ToStdString(), ItemDefinition->GetName().ToStdString(), FocusedSlot);
        OutDroppedDefinition = FocusedDefinition;
        return FocusedSlot;
    }

    // How close two pickups of the same item must lie to merge, and how long the one flies into the other. The search
    // that picks them is server code no client build has (FindExistingPickupNear), so both are chosen: pickups are 60 across.
    // A player's drop is tossed 512 ahead of the pawn and falls short (TossOntoFloor), coming down 60 to 140 ahead, so two
    // drops from one spot facing different ways lie up to 280 apart: at 200, a second drop made after turning round
    // (135 to 180 degrees) came down 230 to 280 from the first and stayed apart. 300 reaches any two drops from one spot,
    // also with the drop's spread to the side (GetDropTossLocation: up to 40 either way, measured).
    constexpr float CombineRadius = 300.0f;
    constexpr float CombineFlyTime = 0.25f;

    // A combine in flight: the pickup flying into its target, the count it brings, and the timer that ends it when
    // CombineFlyTime is up (AFortPickup::EndCombine), with the serial its timer calls back with.
    struct FPickupCombine
    {
        TWeakObjectPtr<AFortPickup> Pickup;
        TWeakObjectPtr<AFortPickup> Target;
        int32_t Count;
        FTimerHandle TimerHandle;
        uint64_t Serial;
    };
    std::vector<FPickupCombine> PickupCombines;

    // What the pickups flying into Target bring.
    int32_t GetIncomingCount(const AFortPickup* Target)
    {
        int32_t Count = 0;
        for (const FPickupCombine& Combine : PickupCombines)
        {
            if (Combine.Target.Get(true) == Target)
            {
                Count += Combine.Count;
            }
        }
        return Count;
    }

    // Room for any build's FHitResult (0x88 bytes in UE 4.21), for a delegate parameter Core doesn't get.
    constexpr size_t HitResultStorageSize = 0x200;

    // The pickups at rest that can take more, by item and cell of CombineRadius, so a pickup coming to rest looks only at
    // the cells around it (AFortPickup::FindExistingPickupNear). A pickup goes in where its toss completes and stays
    // there; those gone, picked up, full or flying into another drop out as their cells are looked at.
    std::unordered_map<uint64_t, std::vector<TWeakObjectPtr<AFortPickup>>> RestingPickupCells;

    // The fallback's tossed pickups (AFortPickup::WatchTossedPickups).
    std::vector<TWeakObjectPtr<AFortPickup>> TossedPickups;
    bool bWatchTossedPickupsQueued = false;

    // Where a native Core added for a delegate (or the build's own OnServerStopCallback) finds its parameters: the
    // broadcast (ProcessMulticastDelegate) calls it through ProcessEvent, which copies the signature's parameter struct
    // to the frame's Locals. A parameter passed by reference (the hit, the impact velocity) isn't copied out by
    // stepping (StepExplicitProperty only points at it), so it is read there, at its property's offset.
    const void* GetDelegateParameter(const FFrame& Stack, const UFunction* Function, int32_t Index)
    {
        const UField* Field = Function ? static_cast<UField*>(Function->Children) : nullptr;
        for (; Field && Index > 0; --Index)
        {
            Field = Field->Next;
        }
        uint8_t* const Locals = Natives::FFrame_Locals >= 0 ? Stack.Locals : nullptr;
        return Field && Locals ? Locals + reinterpret_cast<const UProperty*>(Field)->GetOffset_ForInternal() : nullptr;
    }

    // The signature of a class's BlueprintAssignable delegate (a UMulticastDelegateProperty's SignatureFunction), or null.
    const UFunction* GetDelegateSignature(const UClass* Class, const wchar_t* PropertyName)
    {
        const UProperty* Property = Class ? Class->FindPropertyByName(FName(PropertyName, FNAME_Find)) : nullptr;
        return Property && Property->GetClass()->HasAnyCastFlag(CASTCLASS_UMulticastDelegateProperty)
            ? static_cast<UFunction*>(static_cast<const UMulticastDelegateProperty*>(Property)->SignatureFunction) : nullptr;
    }

    int32_t GetPickupMaxStackSize(const FFortItemEntry& Entry)
    {
        const UFortItemDefinition* Definition = Entry.ItemDefinition;
        return Definition ? std::max(static_cast<int32_t>(Definition->MaxStackSize), 1) : 1;
    }

    // Whether a pickup's toss is over. The toss (0x6622F0) sets TossState InProgress before it returns, but AtRest only
    // comes 5s after the pickup stops: outside a pickup target or a combine, the stop (0x662120) is called from the
    // pickup's tick once its movement has had no velocity for 5s (0x684A70). Measured on a 1.7.2 server, a drop lay still
    // 0.65s after it was tossed and was AtRest at 5.5s, so a second drop lay apart from the first for 5.5s before it
    // merged. The projectile stops simulating when it comes to rest (StopSimulating clears its UpdatedComponent, then
    // broadcasts OnProjectileStop: OnServerStopCallback) and doesn't move again, so that ends the toss too: a second drop
    // merges about 0.9s after it is made.
    bool IsTossOver(AFortPickup* Pickup)
    {
        UProjectileMovementComponent* Movement = Pickup->MovementComponent;
        return static_cast<FFortPickupLocationData&>(Pickup->PickupLocationData).TossState != EFortPickupTossState::GetInProgress() || (Movement && Movement->HasStoppedSimulating());
    }

    // A pickup that can merge or take another: one item that stacks, short of a full stack, at rest, not picked up and
    // not flying into another.
    bool CanCombine(AFortPickup* Pickup)
    {
        FFortItemEntry& Entry = Pickup->PrimaryPickupItemEntry;
        FFortPickupLocationData& LocationData = Pickup->PickupLocationData;
        const int32_t MaxStackSize = GetPickupMaxStackSize(Entry);
        return Pickup->Role == ROLE_Authority && !Pickup->bPickedUp && !LocationData.CombineTarget && IsTossOver(Pickup)
            && Pickup->MultiItemPickupEntries.Num() == 0 && MaxStackSize > 1 && Entry.Count > 0 && Entry.Count < MaxStackSize;
    }

    // Whether two pickups hold the same stack: item, level and durability (AFortInventory::CanStackWith's rules for an
    // inventory, 1.7.2 0x62E040), for the same owner and mission (Save the World's loot can be for one player).
    bool IsSameStack(AFortPickup* Pickup, AFortPickup* Other)
    {
        FFortItemEntry& Entry = Pickup->PrimaryPickupItemEntry;
        FFortItemEntry& OtherEntry = Other->PrimaryPickupItemEntry;
        return static_cast<UFortItemDefinition*>(Entry.ItemDefinition) == static_cast<UFortItemDefinition*>(OtherEntry.ItemDefinition) && Entry.Level == OtherEntry.Level
            && Entry.Durability == OtherEntry.Durability && Pickup->OptionalOwnerID == Other->OptionalOwnerID && Pickup->OptionalMissionGuid == Other->OptionalMissionGuid;
    }

    // Where a pickup is: its root component's location, which is the world location for a root with no parent (a
    // pickup's collision cylinder), without the call GetTransform makes for each of the world's pickups.
    FVector GetPickupLocation(AFortPickup* Pickup)
    {
        USceneComponent* Root = Pickup->GetRootComponent();
        return Root && !Root->AttachParent ? static_cast<FVector>(Root->RelativeLocation) : Pickup->GetTransform().GetLocation();
    }

    uint64_t CombineCell(const UFortItemDefinition* Definition, const FVector& Location, int32_t OffsetX, int32_t OffsetY)
    {
        const int64_t CellX = static_cast<int64_t>(std::floor(Location.X / CombineRadius)) + OffsetX;
        const int64_t CellY = static_cast<int64_t>(std::floor(Location.Y / CombineRadius)) + OffsetY;
        return reinterpret_cast<uintptr_t>(Definition) ^ (static_cast<uint64_t>(CellX) * 0x9E3779B97F4A7C15ull) ^ (static_cast<uint64_t>(CellY) * 0xC2B2AE3D27D4EB4Full);
    }

    // ETInteractionType, a namespaced enum: its UEnum is /Script/FortniteGame.TInteractionType in every build (1.1 to
    // 1.8.2). Looked up as ETInteractionType it wasn't found, IT_Simple came out as 0xFF and every pickup's CanInteract,
    // which refuses anything but IT_Simple (1), said no: on 1.8.2 walking over ammo picked nothing up (#135).
    class TInteractionType
    {
        DefineUnrealEnum(TInteractionType, FortniteGame)
        DefineUEnumValue(IT_Simple)
    };

    // What an interaction asks an IFortInteractInterface about (0x20 bytes in 1.1 and 1.2, 0x28 from 1.7.2).
    struct FInteractionType
    {
        DefineUnrealStruct(FInteractionType, FortniteGame)

    public:
        DefineUProperty(uint8_t, InteractionType)
        DefineUProperty(FWeakObjectPtr, RequestingPawn)
        DefineUProperty(FWeakObjectPtr, InteractComponent)
        DefineUProperty(FVector, InteractPoint)

        static ptrdiff_t RequestingPawnOffset() { return Member_RequestingPawn.GetOffsetChecked(); }
        static ptrdiff_t InteractComponentOffset() { return Member_InteractComponent.GetOffsetChecked(); }
    };
}

void AFortPickup::CombineItemEntries(TStructArray<FFortItemEntry>& Entries, AFortInventory* Inventory)
{
    // UFortLootPackage::PickLootDrops (as the game's) only merges items that can't take more than one stack (resources,
    // ammo), so every roll of an ingredient such as planks stays its own entry. Core merges them before spawning, so a
    // drop is one pickup per stack; pickups that land near each other merge afterwards too (CombinePickups). Weapons and
    // other single items keep a stack size of 1 and stay apart.
    for (int32_t Index = 0; Index < Entries.Num(); ++Index)
    {
        FFortItemEntry& Entry = Entries[Index];
        const UFortItemDefinition* Definition = Entry.ItemDefinition;
        const int32_t MaxStackSize = Definition && Inventory ? std::max(Inventory->GetMaxStackSize(Definition), 1) : GetPickupMaxStackSize(Entry);
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
    if (Natives::AFortPickup_CreateFromData)
    {
        // 4.1 on. bRandomRotation stays off: Core's callers set it themselves when they want it.
        FFortCreatePickupData PickupData;
        PickupData.World = World;
        PickupData.ItemEntry = &ItemEntry;
        PickupData.SpawnLocation = SpawnLocation;
        PickupData.SpawnRotation = SpawnRotation;
        PickupData.PlayerController = OptionalOwner;
        PickupData.OverrideClass = OverrideClass;
        return Natives::AFortPickup_CreateFromData(PickupData);
    }
    return Natives::AFortPickup_CreatePickup(World, ItemEntry, SpawnLocation, SpawnRotation, OptionalOwner, OverrideClass, 0, 0, 0);
}

void AFortPickup::TossPickup(const FVector& FinalLocation, AFortPawn* ItemOwner, int32_t OverrideMaxStackCount, bool bToss)
{
    // 1.8 added bToss (execTossPickup, 0xB47C70, reads four parameters and passes the last to 0x698240). Without it the
    // toss (0x67D9E0, AFortPickup slot 0x678) starts the pickup at FinalLocation instead of where it is, so its launch
    // covers no distance: every pickup Core tossed on 1.8 appeared on its spot instead of flying there (#108). Core's
    // tosses left the parameter at zero, as it is a parameter 1.1 to 1.7.2 don't have.
    static const bool bHasToss = StaticFunction_TossPickup()->FindPropertyByName(FName(L"bToss", FNAME_Find)) != nullptr;
    Reflection::FParams Params(StaticFunction_TossPickup());
    Params.Set(L"FinalLocation", FinalLocation);
    Params.Set(L"ItemOwner", ItemOwner);
    Params.Set(L"OverrideMaxStackCount", OverrideMaxStackCount);
    if (bHasToss)
    {
        Params.Set(L"bToss", bToss);
    }
    Params.Call(this);
#if WITH_SERVER_CODE
    // Core's tosses merge into a pickup of the same stack near where they land (CombinePickups), as 21.40's server sets
    // a toss to (SetCombinePickupsWhenTossCompletes).
    if (Role == ROLE_Authority)
    {
        SetCombinePickupsWhenTossCompletes(true);
        BindServerMovementCallbacks();
    }
#endif
}

void AFortPickup::TossFromContainer(const FVector& FinalLocation, const FVector& LaunchVelocity)
{
    // A toss (1.7.2 0x6622F0, run by 0x680570 on the server) launches the pickup's projectile movement at 45 degrees with
    // a horizontal speed of the distance over the fly time, and clients play it from the replicated movement (the
    // pickup's PostNetReceiveVelocity, 0x67BAD0, feeds the projectile). A container's loot leaves at the container's own
    // toss velocity instead and lands where its bounces take it, and bTossedFromContainer tells clients
    // (OnRep_TossedFromContainer, 0x67A830: the pickup effect's OnTossed and the touch collision).
    TossPickup(FinalLocation, nullptr, 0);
    UProjectileMovementComponent* Movement = MovementComponent;
    if (Movement && Role == ROLE_Authority)
    {
        Movement->Velocity = LaunchVelocity;
        Movement->MaxSpeed = LaunchVelocity.Size();
    }
    if (Member_bTossedFromContainer.IsAvailable())
    {
        bTossedFromContainer = true;
    }
}

void AFortPickup::TossOntoFloor(const FVector& FloorLocation, AFortPawn* ItemOwner, float LaunchUpSpeed)
{
    // A toss doesn't reach the location it is given (#89). 0x680570 launches at 45 degrees with a horizontal speed of the
    // distance over the fly time (about 1.4s) and drops the gravity its solver (0x2A7160) works out for that flight, so the
    // pickup falls under the world's (2800 in Battle Royale): from a body's height, aimed 1.5 to 2.5m out, it came down
    // 10 to 35 out, and the items lay in a pile. The launch is replaced with one solved for the movement's own gravity: up
    // Rise over the higher of the start and the landing, down onto the floor with the pickup's collision resting on it
    // (its root is 80 above it), with the horizontal speed spread over the flight and the hops that follow (each bounce
    // keeps Bounciness of the vertical speed and 1 - Friction of the horizontal: 0.6 and 0.5). Measured on a 1.7.2 server,
    // pickups come to rest within 10 of their spot, after about a second. One aimed past a ledge still lands on the upper
    // floor, short of it. Given a LaunchUpSpeed, it rises as far as that speed takes it instead of Rise.
    constexpr float DefaultRise = 70.0f;
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
    const float Rise = LaunchUpSpeed > 0.0f ? LaunchUpSpeed * LaunchUpSpeed / (2.0f * Gravity) : DefaultRise;
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

bool AFortPickup::CanInteract(AFortPlayerPawn* RequestingPawn)
{
    static UClass* InteractInterface = FindObject<UClass>(nullptr, L"/Script/FortniteGame.FortInteractInterface");
    if (!RequestingPawn || !InteractInterface || !Finder::ResolvePickupCanInteractLive(PickedUpOffset()))
    {
        return false;
    }
    void* Interface = Natives::UObject_GetInterfaceAddress(this, InteractInterface);
    UScriptStruct* InteractionStruct = FInteractionType::StaticStruct();
    const uint8_t Simple = TInteractionType::GetIT_Simple();
    if (!Interface || !InteractionStruct || Simple == Reflection::InvalidEnumValue)
    {
        static bool bReported = false;
        if (!bReported)
        {
            bReported = true;
            Log::Error(L"Pickup: can't ask {} whether it can be picked up (interface {}, FInteractionType {}, IT_Simple {})", GetName().ToStdString(), Interface != nullptr, InteractionStruct != nullptr, Simple);
        }
        return false;
    }

    // As FInteractionType's constructor builds it for AFortPlayerPawnAthena's overlap (1.8.1 0x5DC180): the type, the
    // pawn, no component and no point. From 1.7.2 an unreflected weak pointer sits between the pawn and the component,
    // and the constructor points it at the pawn too.
    std::vector<uint8_t> Storage(InteractionStruct->GetStructureSize(), 0);
    FInteractionType* Interaction = reinterpret_cast<FInteractionType*>(Storage.data());
    InteractionStruct->InitializeStruct(Interaction);
    Interaction->InteractionType = Simple;
    Interaction->RequestingPawn = FWeakObjectPtr(RequestingPawn);
    Interaction->InteractComponent = FWeakObjectPtr();
    Interaction->InteractPoint = FVector(0.0f, 0.0f, 0.0f);
    const ptrdiff_t SecondPawn = FInteractionType::RequestingPawnOffset() + static_cast<ptrdiff_t>(sizeof(FWeakObjectPtr));
    if (FInteractionType::InteractComponentOffset() - SecondPawn == static_cast<ptrdiff_t>(sizeof(FWeakObjectPtr)))
    {
        *reinterpret_cast<FWeakObjectPtr*>(Storage.data() + SecondPawn) = FWeakObjectPtr(RequestingPawn);
    }
    const bool bCanInteract = CallVirtual<bool>(Interface, Natives::IFortInteractInterface_CanInteractIndex, static_cast<const void*>(Interaction));
    InteractionStruct->DestroyStruct(Interaction);
    return bCanInteract;
}

void AFortPickup::SetOptionalMissionGuid(const FGuid& InMissionGuid)
{
    if (Role == ROLE_Authority)
    {
        OptionalMissionGuid = InMissionGuid;
    }
}

bool FFortPickupLocationData::IsCombineAvailable()
{
    return Member_CombineTarget.IsAvailable() && Member_LootInitialPosition.IsAvailable() && Member_FlyTime.IsAvailable() && Member_TossState.IsAvailable();
}

bool FFortPickupLocationData::CanRestAt()
{
    static const bool bCanRestAt = Member_FinalTossRestLocation.IsAvailable() && EFortPickupTossState::GetAtRest() != Reflection::InvalidEnumValue;
    return bCanRestAt;
}

void AFortPickup::OnRep_PickupLocationData()
{
    Reflection::FParams Params(StaticFunction_OnRep_PickupLocationData());
    Params.Call(this);
}

void AFortPickup::SetCombinePickupsWhenTossCompletes(bool bInCombinePickupsWhenTossCompletes)
{
    if (Member_bCombinePickupsWhenTossCompletes.IsAvailable())
    {
        bCombinePickupsWhenTossCompletes = bInCombinePickupsWhenTossCompletes;
    }
}

void AFortPickup::BindServerMovementCallbacks()
{
#if WITH_SERVER_CODE
    if (Role != ROLE_Authority)
    {
        return;
    }
    // 4.0's PostInitializeComponents binds the server's stop callback as the pickup spawns; that function is intact and
    // Core doesn't hook it, so Core binds the callbacks the build doesn't as it tosses the pickup, before the toss can end.
    UProjectileMovementComponent* Movement = MovementComponent;
    if (bBindServerStopCallback && Movement)
    {
        FMulticastScriptDelegate& StopDelegate = Movement->OnProjectileStop;
        StopDelegate.AddUniqueDynamic(this, OnServerStopCallbackFunction->GetFName());
    }
    else if (!OnServerStopCallbackFunction)
    {
        // Nothing can tell Core the toss ended (WatchTossedPickups says why).
        TossedPickups.push_back(this);
        if (!bWatchTossedPickupsQueued)
        {
            bWatchTossedPickupsQueued = GameThread::Enqueue(&AFortPickup::WatchTossedPickups);
        }
    }
    if (OnServerBounceCallbackFunction && Movement)
    {
        FMulticastScriptDelegate& BounceDelegate = Movement->OnProjectileBounce;
        BounceDelegate.AddUniqueDynamic(this, OnServerBounceCallbackFunction->GetFName());
    }
#endif
}

void AFortPickup::OnServerStopCallback(const FHitResult& Hit)
{
    // 4.0 on, the build's own (4.1 0xD87880, 4.5 0xDC2DB0, 5.40 0xFA2520), all of it a client build keeps: on the server,
    // with a movement component, bServerStoppedSimulation goes on, and its OnRep stops a client's simulation of the toss.
    if (OnServerStopCallbackIndex >= 0)
    {
        CallVirtual<void>(this, OnServerStopCallbackIndex, &Hit);
    }
#if WITH_SERVER_CODE
    // The rest is server code: a toss that combines when it completes merges into a pickup near where it came to rest.
    // Before 4.0 Core binds this to its own tosses alone, and each of those combines.
    if (Role == ROLE_Authority && (!Member_bCombinePickupsWhenTossCompletes.IsAvailable() || bCombinePickupsWhenTossCompletes))
    {
        CombinePickups();
    }
#endif
}

void AFortPickup::OnServerBounceCallback(const FHitResult& Hit, const FVector& ImpactVelocity)
{
#if WITH_SERVER_CODE
    // 4.5 to 5.40 have no OnServerBounceCallback (21.40's, bound to the movement's OnProjectileBounce on the server), and
    // nothing in a client build bumps ServerImpactSoundFlash: the bump is server code. The pickup's NotifyHit decides on
    // the landed sound (4.5 0xDC1410, 5.40 0xFA0A60): a hit the pickup moved itself into, a second after the last sound,
    // and on 5.40 one whose Z force passes LandSoundZForceThreshold; where it plays the sound it sets
    // LastLandedSoundPlayTime to the world's time. The server's own NotifyHit runs for the hit as the move that made it
    // ends, before the movement's HandleImpact broadcasts the bounce, so a bounce whose hit played the sound finds
    // LastLandedSoundPlayTime at this moment's time, and clients that don't simulate the pickup play it from the flash.
    UWorld* World = GetWorld();
    if (Role != ROLE_Authority || !World || LastLandedSoundPlayTime != World->GetTimeSeconds())
    {
        return;
    }
    ServerImpactSoundFlash = static_cast<uint8_t>(ServerImpactSoundFlash + 1);
#endif
}

void AFortPickup::CombinePickups(const AFortPickup* Except)
{
#if WITH_SERVER_CODE
    // Pickups of the same item that come to rest near each other merge into one (pickup combining did nothing). The client
    // half is in every build: PickupLocationData's CombineTarget sends OnRep_PickupLocationData (1.7.2 0x67A7A0; 1.2
    // 0x1405EA760, 1.8 0x690DD0) to its combine branch (0x681C30), which stops the toss (0x662120) and sets a timer for
    // FlyTime, and the pickup's tick (0x6832A0 -> 0x684B10) moves it from LootInitialPosition into the target over
    // FlyTime; while it has a target it can't be picked up (0x6783E0). The server half is server code a client build
    // doesn't have: the timer is bound to the shared empty function, and nothing sets a CombineTarget. It starts as the
    // toss completes (OnServerStopCallback): the pickup flies into the nearest resting pickup of the same stack within
    // CombineRadius with room for it (FindExistingPickupNear), and when FlyTime is up the target takes what fits of its
    // count (EndCombine). What doesn't fit stays in the pickup, which goes back to where it landed (CancelCombine). A
    // landed pickup stays one of the resting ones, so two pickups are compared when the later of them comes to rest,
    // wherever the earlier one landed.
    static const bool bCombineAvailable = []()
    {
        const bool bAvailable = Member_PickupLocationData.IsAvailable() && Member_bPickedUp.IsAvailable() && StaticFunction_OnRep_PickupLocationData()
            && FFortPickupLocationData::IsCombineAvailable() && EFortPickupTossState::GetInProgress() != Reflection::InvalidEnumValue;
        if (!bAvailable)
        {
            Log::Error(L"Pickup: this build doesn't reflect what pickup combining uses (PickupLocationData's CombineTarget, LootInitialPosition, FlyTime, TossState), pickups won't combine");
        }
        return bAvailable;
    }();
    if (!bCombineAvailable || !CanCombine(this))
    {
        return;
    }

    // One of the resting pickups from now on, in the cell it lies in.
    const UFortItemDefinition* Definition = static_cast<FFortItemEntry&>(PrimaryPickupItemEntry).ItemDefinition;
    std::vector<TWeakObjectPtr<AFortPickup>>& Cell = RestingPickupCells[CombineCell(Definition, GetPickupLocation(this), 0, 0)];
    const TWeakObjectPtr<AFortPickup> WeakThis(this);
    if (std::find(Cell.begin(), Cell.end(), WeakThis) == Cell.end())
    {
        Cell.push_back(WeakThis);
    }

    // A pickup that others fly into stays.
    if (GetIncomingCount(this) > 0)
    {
        return;
    }
    if (AFortPickup* Target = FindExistingPickupNear(Except))
    {
        StartCombine(Target);
    }
#endif
}

AFortPickup* AFortPickup::FindExistingPickupNear(const AFortPickup* Except)
{
    FFortItemEntry& Entry = PrimaryPickupItemEntry;
    const UFortItemDefinition* Definition = Entry.ItemDefinition;
    const int32_t MaxStackSize = GetPickupMaxStackSize(Entry);
    const FVector Location = GetPickupLocation(this);
    AFortPickup* Nearest = nullptr;
    float NearestDistanceSquared = CombineRadius * CombineRadius;
    // The closest pickup of the item it passed over, and why, for the line below when a pawn's drop merges with none
    // (loot lying a little apart from loot of the same item is common, and stays quiet).
    AFortPickup* Missed = nullptr;
    float MissedDistanceSquared = 4.0f * CombineRadius * CombineRadius;
    const wchar_t* MissedReason = nullptr;
    for (int32_t OffsetX = -1; OffsetX <= 1; ++OffsetX)
    {
        for (int32_t OffsetY = -1; OffsetY <= 1; ++OffsetY)
        {
            const auto Cell = RestingPickupCells.find(CombineCell(Definition, Location, OffsetX, OffsetY));
            if (Cell == RestingPickupCells.end())
            {
                continue;
            }
            std::erase_if(Cell->second, [](const TWeakObjectPtr<AFortPickup>& Resting)
            {
                AFortPickup* Pickup = Resting.Get();
                return !Pickup || !CanCombine(Pickup);
            });
            for (const TWeakObjectPtr<AFortPickup>& Resting : Cell->second)
            {
                AFortPickup* Other = Resting.Get();
                if (Other == this || Other == Except)
                {
                    continue;
                }
                const FVector OtherLocation = GetPickupLocation(Other);
                const float DistanceSquared = FMath::Square(OtherLocation.X - Location.X) + FMath::Square(OtherLocation.Y - Location.Y) + FMath::Square(OtherLocation.Z - Location.Z);
                const int32_t OtherCount = static_cast<FFortItemEntry&>(Other->PrimaryPickupItemEntry).Count + GetIncomingCount(Other);
                const wchar_t* Reason = DistanceSquared > CombineRadius * CombineRadius ? L"out of reach"
                    : OtherCount >= MaxStackSize ? L"full"
                    : !IsSameStack(this, Other) ? L"another item, level, durability, owner or mission"
                    : nullptr;
                if (!Reason && DistanceSquared <= NearestDistanceSquared)
                {
                    Nearest = Other;
                    NearestDistanceSquared = DistanceSquared;
                }
                else if (Reason && DistanceSquared < MissedDistanceSquared)
                {
                    Missed = Other;
                    MissedDistanceSquared = DistanceSquared;
                    MissedReason = Reason;
                }
            }
            if (Cell->second.empty())
            {
                RestingPickupCells.erase(Cell);
            }
        }
    }
    if (!Nearest && Missed && static_cast<AFortPawn*>(PawnWhoDroppedPickup))
    {
        Log::Info(L"Pickup: {} {} came to rest {:.0f} from {} and stays apart ({})", static_cast<int32_t>(Entry.Count), Definition ? Definition->GetName().ToStdString() : L"None", std::sqrt(MissedDistanceSquared), Missed->GetName().ToStdString(), MissedReason);
    }
    return Nearest;
}

void AFortPickup::StartCombine(AFortPickup* Target)
{
    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }
    FFortPickupLocationData& LocationData = PickupLocationData;
    LocationData.CombineTarget = Target;
    LocationData.LootInitialPosition = GetTransform().GetLocation();
    LocationData.FlyTime = CombineFlyTime;
    OnRep_PickupLocationData();
    ForceNetUpdate();

    // The combine branch's timer for FlyTime runs here too, but it is bound to server code a client build doesn't have
    // (the shared empty function): Core's ends the combine (EndCombine). The serial tells a combine from a later one of
    // the same pickup.
    static uint64_t LastSerial = 0;
    const uint64_t Serial = ++LastSerial;
    FPickupCombine Combine{ this, Target, static_cast<FFortItemEntry&>(PrimaryPickupItemEntry).Count, {}, Serial };
    World->GetTimerManager().SetTimer(Combine.TimerHandle, FTimerDelegate::CreateLambda([WeakPickup = TWeakObjectPtr<AFortPickup>(this), Serial]()
    {
        if (AFortPickup* Pickup = WeakPickup.Get())
        {
            Pickup->EndCombine(Serial);
        }
        else
        {
            std::erase_if(PickupCombines, [Serial](const FPickupCombine& Ended) { return Ended.Serial == Serial; });
        }
    }), CombineFlyTime, false);
    PickupCombines.push_back(Combine);

    // A pawn starting to pick the target up sends this one back at once (HandleCombineTargetPickupAttempted): the
    // pickup's tick moves it toward the target as it is now (5.0 CL-4204761 0xF55420: VInterpTo from LootInitialPosition
    // to the CombineTarget's root, then SetActorLocation), on the server and on each client, so it would follow the
    // target to the pawn (#344: light bullets from a chest flying into floor ammo the player then walked over ended up at
    // the player).
    if (HandleCombineTargetPickupAttemptedFunction)
    {
        FMulticastScriptDelegate& TargetPickupAttempted = Target->OnPickupAttempted;
        TargetPickupAttempted.AddUniqueDynamic(this, HandleCombineTargetPickupAttemptedFunction->GetFName());
    }
}

AFortPickup* AFortPickup::TakeCombine(bool& bOutFound, uint64_t Serial)
{
    bOutFound = false;
    const auto Found = std::find_if(PickupCombines.begin(), PickupCombines.end(), [this, Serial](const FPickupCombine& Combine)
    {
        return Combine.Pickup.Get(true) == this && (Serial == 0 || Combine.Serial == Serial);
    });
    if (Found == PickupCombines.end())
    {
        return nullptr;
    }
    bOutFound = true;
    AFortPickup* Target = Found->Target.Get();
    FTimerHandle TimerHandle = Found->TimerHandle;
    PickupCombines.erase(Found);
    // The timer's own call (Serial) has nothing left to clear; an early end clears it (InRate 0).
    UWorld* World = Serial == 0 && TimerHandle.IsValid() ? GetWorld() : nullptr;
    if (World)
    {
        World->GetTimerManager().SetTimer(TimerHandle, FTimerDelegate(), 0.0f, false);
    }
    if (Target && HandleCombineTargetPickupAttemptedFunction)
    {
        FMulticastScriptDelegate& TargetPickupAttempted = Target->OnPickupAttempted;
        TargetPickupAttempted.RemoveDynamic(this, HandleCombineTargetPickupAttemptedFunction->GetFName());
    }
    return Target;
}

void AFortPickup::EndCombine(uint64_t Serial)
{
    bool bFound = false;
    AFortPickup* Target = TakeCombine(bFound, Serial);
    if (!bFound)
    {
        return;
    }
    // A target picked up where its OnPickupAttempted couldn't send this one back, or one that is gone.
    if (!Target || Target->bPickedUp)
    {
        const FFortItemEntry& Entry = PrimaryPickupItemEntry;
        const UFortItemDefinition* Definition = Entry.ItemDefinition;
        Log::Info(L"Pickup: {} {} was flying into {}, which {}, so it goes back", static_cast<int32_t>(Entry.Count), Definition ? Definition->GetName().ToStdString() : L"None",
            Target ? Target->GetName().ToStdString() : L"a pickup", Target ? L"was picked up" : L"is gone");
        CancelCombine();
        return;
    }
    FinishCombine(Target);
}

void AFortPickup::FinishCombine(AFortPickup* Target)
{
    FFortItemEntry& Entry = PrimaryPickupItemEntry;
    const UFortItemDefinition* Definition = Entry.ItemDefinition;
    const int32_t Count = Entry.Count;
    int32_t Moved = 0;
    if (Target && Target->Role == ROLE_Authority && !Target->bPickedUp && !static_cast<FFortPickupLocationData&>(Target->PickupLocationData).CombineTarget && IsSameStack(this, Target))
    {
        FFortItemEntry& TargetEntry = Target->PrimaryPickupItemEntry;
        Moved = std::clamp(GetPickupMaxStackSize(TargetEntry) - static_cast<int32_t>(TargetEntry.Count), 0, Count);
        if (Moved > 0)
        {
            TargetEntry.Count = TargetEntry.Count + Moved;
            Target->ForceNetUpdate();
        }
    }
    if (Moved >= Count)
    {
        Log::Info(L"Pickup: {} {} combined into {} ({} now)", Count, Definition ? Definition->GetName().ToStdString() : L"None", Target->GetName().ToStdString(), static_cast<int32_t>(static_cast<FFortItemEntry&>(Target->PrimaryPickupItemEntry).Count));
        K2_DestroyActor();
        return;
    }

    Entry.Count = Count - Moved;
    Log::Info(L"Pickup: {} of {} {} combined into {}, the rest goes back", Moved, Count, Definition ? Definition->GetName().ToStdString() : L"None", Target ? Target->GetName().ToStdString() : L"a pickup that is gone");
    CancelCombine();
}

void AFortPickup::CancelCombine(const AFortPickup* Except)
{
    // Off the combines in flight, if it still is: its timer cleared, its target's OnPickupAttempted no longer bound.
    bool bFound = false;
    TakeCombine(bFound, 0);

    // With no target, OnRep_PickupLocationData takes the toss branch (AFortPickup::SetupForMovementCompToss, 4.5 0xDC75B0,
    // 5.0 0xF533F0). A toss still InProgress there is launched again, from where the pickup is, at a velocity solved from
    // LootInitialPosition to LootFinalPosition: a pickup that had followed its target to a player was thrown out from the
    // player (#344). A toss AtRest stops the movement and puts the pickup on FinalTossRestLocation, on the server and on
    // each client. Core's combine starts once the pickup lies still (its movement's OnProjectileStop), well before the 5s
    // after which its tick sets AtRest, so it was still InProgress. The pickup now goes back AtRest to where it lay when the
    // combine started (StartCombine's LootInitialPosition).
    FFortPickupLocationData& LocationData = PickupLocationData;
    const FVector RestLocation = LocationData.LootInitialPosition;
    LocationData.CombineTarget = nullptr;
    const bool bReturnAtRest = FFortPickupLocationData::CanRestAt();
    if (bReturnAtRest)
    {
        LocationData.TossState = EFortPickupTossState::GetAtRest();
        LocationData.FinalTossRestLocation = RestLocation;
    }
    OnRep_PickupLocationData();
    if (!bReturnAtRest)
    {
        SetActorLocation(RestLocation, false, true);
    }
    ForceNetUpdate();
    // At rest again where it lay: it may merge into another.
    CombinePickups(Except);
}

void AFortPickup::HandleCombineTargetPickupAttempted(AFortPickup* AttemptedPickup)
{
#if WITH_SERVER_CODE
    // SetPickupTarget broadcasts it before it sets bPickedUp, so the target is left out of the pickup's next search.
    const AFortPickup* CombineTarget = static_cast<FFortPickupLocationData&>(PickupLocationData).CombineTarget;
    if (Role != ROLE_Authority || !AttemptedPickup || CombineTarget != AttemptedPickup)
    {
        if (AttemptedPickup && HandleCombineTargetPickupAttemptedFunction)
        {
            FMulticastScriptDelegate& TargetPickupAttempted = AttemptedPickup->OnPickupAttempted;
            TargetPickupAttempted.RemoveDynamic(this, HandleCombineTargetPickupAttemptedFunction->GetFName());
        }
        return;
    }
    const FFortItemEntry& Entry = PrimaryPickupItemEntry;
    const UFortItemDefinition* Definition = Entry.ItemDefinition;
    Log::Info(L"Pickup: {} {} was flying into {}, which is being picked up, so it goes back", static_cast<int32_t>(Entry.Count), Definition ? Definition->GetName().ToStdString() : L"None",
        AttemptedPickup->GetName().ToStdString());
    CancelCombine(AttemptedPickup);
#endif
}

void AFortPickup::WatchTossedPickups()
{
#if WITH_SERVER_CODE
    // The fallback where nothing tells Core a toss ended: before 4.0 the build has no OnServerStopCallback (4.0's
    // PostInitializeComponents binds it to the movement's OnProjectileStop on the server), and here Core couldn't add its
    // own (no UClass::FuncMap, logged at boot); from 4.0 the build's couldn't be taken over. Core's tossed pickups are
    // looked at each frame until their toss is over: the projectile stops simulating when it comes to rest
    // (StopSimulating clears its UpdatedComponent), or the pickup's tick sets AtRest 5s after it stopped moving (1.7.2
    // 0x684A70 -> 0x662120).
    bWatchTossedPickupsQueued = false;
    std::vector<AFortPickup*> Landed;
    std::erase_if(TossedPickups, [&Landed](const TWeakObjectPtr<AFortPickup>& Tossed)
    {
        AFortPickup* Pickup = Tossed.Get();
        if (Pickup && !Pickup->bPickedUp && !IsTossOver(Pickup))
        {
            return false;
        }
        if (Pickup)
        {
            Landed.push_back(Pickup);
        }
        return true;
    });
    for (AFortPickup* Pickup : Landed)
    {
        Pickup->CombinePickups();
    }
    if (!TossedPickups.empty() && !bWatchTossedPickupsQueued)
    {
        bWatchTossedPickupsQueued = GameThread::Enqueue(&AFortPickup::WatchTossedPickups);
    }
#endif
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
    }
    else if (AFortPlayerController* PlayerController = Cast<AFortPlayerController>(OwnerObject))
    {
        GivePickupToPlayer(PlayerController);
    }

    // Each picked-up item flew in further round to the side than the last (#99). A pickup flying to a pawn adds an entry
    // to the pawn's PickupDirectionData when its path is built (0x681660 -> 0x779E30), at the last entry's angle plus 40,
    // and the entries are then spread evenly over plus and minus half their range, at most 90 (0x7AED90); the path comes
    // into the loot socket along the pawn's down vector rolled by its entry's angle (0x7CC3B0). The entry only goes when
    // the pickup is destroyed (AFortPickup's Destroyed, 0x6635A0 -> 0x6621E0 -> 0x7A7EC0), on the server and on each
    // client. Neither SetPickupTarget (0x67FDD0) nor the arrival (FinishedTargetSpline, 0x6685D0, which takes it off the
    // pawn's IncomingPickups and removes its effect before calling this) destroys it, and Core's give never did, so every
    // pickup stayed on the list and the next came in 20 degrees further round, up to 90. Pickups taken together still
    // fan out, as they do natively.
    K2_DestroyActor();
#endif
}

void AFortPickup::GivePickupToPlayer(AFortPlayerController* PlayerController)
{
#if WITH_SERVER_CODE
    std::vector<FFortItemEntry*> Entries = GetPickupEntries(this);
    AFortPawn* Pawn = PlayerController->MyFortPawn;
    UScriptStruct* EntryStruct = FFortItemEntry::StaticStruct();
    int32_t SwappedSlot = -1;
    const UFortItemDefinition* DroppedDefinition = nullptr;
    // An item cache (4.0) isn't an item the player carries: the world manager keeps it for the end of the mission, when
    // EndPrimaryMission's EarnedItemCaches has the MCP give its card pack (AFortWorldManager::AddEarnedItemCache).
    UWorld* World = GetWorld();
    AFortGameState* GameState = World ? Cast<AFortGameState>(reinterpret_cast<UObject*>(World->GetGameState())) : nullptr;
    AFortWorldManager* WorldManager = GameState && AFortWorldManager::HasEarnedItemCaches() ? static_cast<AFortWorldManager*>(GameState->WorldManager) : nullptr;
    for (FFortItemEntry* Entry : Entries)
    {
        if (UFortItemCacheItemDefinition* ItemCache = WorldManager ? Cast<UFortItemCacheItemDefinition>(static_cast<UFortItemDefinition*>(Entry->ItemDefinition)) : nullptr)
        {
            int32_t Earned = 0;
            while (Earned < Entry->Count && WorldManager->AddEarnedItemCache(PlayerController, ItemCache))
            {
                ++Earned;
            }
            if (Earned > 0)
            {
                UFortItemCollectedParams::SendItemCollected(PlayerController, ItemCache, Earned);
                continue;
            }
        }

        int32_t EntrySwappedSlot = -1;
        if (SwappedSlot < 0)
        {
            SwappedSlot = EntrySwappedSlot = DropFocusedItemForSwap(PlayerController, *Entry, DroppedDefinition);
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
        // A leftover lands at the player's feet: said, so a pickup that "appears" at a player can be told apart (#344).
        const UFortItemDefinition* LeftoverDefinition = Entry->ItemDefinition;
        Log::Info(L"Pickup: {} of {}'s {} {} went into the inventory, the other {} go back down at the pawn", Given, PlayerController->GetName().ToStdString(), static_cast<int32_t>(Entry->Count),
            LeftoverDefinition ? LeftoverDefinition->GetName().ToStdString() : L"None", static_cast<int32_t>(Leftover->Count));

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
        if (const TArray<FGuid>& SwappedItems = Bars->GetSlot(Primary, SwappedSlot).Items; SwappedItems.Num() > 0)
        {
            Bars->ServerActivateSlotInternal(Primary, SwappedSlot, 0.0f, false);
            // The client's own quickbar (3.5) leaves the emptied slot for the next one: the activation's execute can be
            // dropped there, and the server's quickbar can't keep the slot focused for it (#269).
            if (!Bars->IsReplicatedToOwner())
            {
                Bars->SetSwappedInItem(SwappedItems[0], DroppedDefinition);
            }
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

ptrdiff_t AFortPickup::PickedUpOffset()
{
    return Member_bPickedUp.GetOffsetChecked();
}

DEFINE_FUNCTION(AFortPickup::execOnServerStopCallback)
{
    // The hit, the signature's one parameter, by reference: read where ProcessEvent put it (GetDelegateParameter).
    // Neither the build's part nor Core's reads it.
    alignas(16) static const uint8_t NoHit[HitResultStorageSize] = {};
    const void* Hit = GetDelegateParameter(P_STACK, OnServerStopCallbackFunction, 0);
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<AFortPickup*>(Context)->OnServerStopCallback(*static_cast<const FHitResult*>(Hit ? Hit : NoHit));
    P_NATIVE_END;
}

DEFINE_FUNCTION(AFortPickup::execOnServerBounceCallback)
{
    // FOnProjectileBounceDelegate's hit and impact velocity, both by reference.
    alignas(16) static const uint8_t NoHit[HitResultStorageSize] = {};
    static const FVector NoVelocity(0.0f, 0.0f, 0.0f);
    const void* Hit = GetDelegateParameter(P_STACK, OnServerBounceCallbackFunction, 0);
    const void* ImpactVelocity = GetDelegateParameter(P_STACK, OnServerBounceCallbackFunction, 1);
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<AFortPickup*>(Context)->OnServerBounceCallback(*static_cast<const FHitResult*>(Hit ? Hit : NoHit), ImpactVelocity ? *static_cast<const FVector*>(ImpactVelocity) : NoVelocity);
    P_NATIVE_END;
}

DEFINE_FUNCTION(AFortPickup::execHandleCombineTargetPickupAttempted)
{
    // OnPickupAttempted's first parameter, the pickup a pawn is picking up; the pawn (and the item and where it goes,
    // where the build passes them) aren't read.
    P_GET_OBJECT(AFortPickup, Z_Param_Pickup);
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<AFortPickup*>(Context)->HandleCombineTargetPickupAttempted(Z_Param_Pickup);
    P_NATIVE_END;
}

void AFortPickup::HookNatives()
{
#if WITH_SERVER_CODE
    UClass* PickupClass = StaticClass();
    const UClass* MovementClass = UProjectileMovementComponent::StaticClass();
    if (!PickupClass || !MovementClass)
    {
        return;
    }

    // A toss's end on the server. From 4.0 the pickup's PostInitializeComponents (4.1 0xD70660, 4.5 0xDAB1E0, 5.40
    // 0xF903B0) binds OnServerStopCallback to its movement's OnProjectileStop where it has authority
    // (AddDynamic(this, &AFortPickup::OnServerStopCallback)), and StopSimulating's broadcast reaches it through the
    // UFunction's exec (5.40 0x14BB4F0), which calls the virtual (5.40 slot 0xC6). Core's exec takes the UFunction's place
    // and calls Core's OnServerStopCallback, which runs the build's virtual first.
    if (UFunction* StopFunction = FindStaticFunction_OnServerStopCallback())
    {
        const int32_t Index = Finder::ImplementationIndex(StopFunction);
        if (Index >= 0 && (Natives::FFrame_Locals >= 0 || Finder::ResolveScriptFrame()))
        {
            OnServerStopCallbackIndex = Index;
            OnServerStopCallbackFunction = StopFunction;
            StopFunction->SetNativeFunc(&execOnServerStopCallback);
            Log::Info(L"Pickup: OnServerStopCallback (slot 0x{:X}) runs Core's server part after the build's", Index);
        }
        else
        {
            Log::Error(L"Finder: AFortPickup::OnServerStopCallback's slot not found (exec 0x{:X})", reinterpret_cast<uintptr_t>(StopFunction->GetNativeFunc()) - Finder::ImageBase());
        }
    }
    // Before 4.0 nothing binds the movement's OnProjectileStop: Core adds OnServerStopCallback, 4.0's name, with the
    // delegate's signature, and binds it to its own tosses (BindServerMovementCallbacks).
    else if (const UFunction* StopSignature = GetDelegateSignature(MovementClass, L"OnProjectileStop"))
    {
        OnServerStopCallbackFunction = PickupClass->AddNativeFunction(L"OnServerStopCallback", &execOnServerStopCallback, StopSignature);
        bBindServerStopCallback = OnServerStopCallbackFunction != nullptr;
    }
    if (!OnServerStopCallbackFunction)
    {
        Log::Warning(L"Pickup: no OnServerStopCallback hears a toss end, so Core's tossed pickups are watched each frame to combine");
    }

    // 4.5 on: ServerImpactSoundFlash, rung from the movement's bounces (OnServerBounceCallback).
    if (Member_ServerImpactSoundFlash.IsAvailable() && Member_LastLandedSoundPlayTime.IsAvailable())
    {
        if (const UFunction* BounceSignature = GetDelegateSignature(MovementClass, L"OnProjectileBounce"))
        {
            OnServerBounceCallbackFunction = PickupClass->AddNativeFunction(L"OnServerBounceCallback", &execOnServerBounceCallback, BounceSignature);
        }
        if (!OnServerBounceCallbackFunction)
        {
            Log::Warning(L"Pickup: OnServerBounceCallback can't be added, so clients that don't simulate a pickup hear no landing");
        }
    }

    // A combine target being picked up sends what flies into it back at once (HandleCombineTargetPickupAttempted). Without
    // OnPickupAttempted it goes back when its fly time is up (EndCombine).
    if (const UFunction* AttemptedSignature = GetDelegateSignature(PickupClass, L"OnPickupAttempted"))
    {
        HandleCombineTargetPickupAttemptedFunction = PickupClass->AddNativeFunction(L"HandleCombineTargetPickupAttempted", &execHandleCombineTargetPickupAttempted, AttemptedSignature);
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
        if (VTable && VTable[GiveIndex] == Natives::AFortPickup_GivePickupToStripped)
        {
            Hooks::SwapVTable(L"AFortPickup::GivePickupTo", VTable, GiveIndex, reinterpret_cast<void*>(&GivePickupToHK));
        }
    }
}

void AFortPickup::GivePickupToHK(AFortPickup* This, IFortInventoryOwnerInterface* InventoryOwner)
{
    This->GivePickupTo(InventoryOwner);
}
