#include "pch.h"
#include "Items/FortQuickBars.h"
#include "Engine/NetDriver.h"
#include "Engine/ReplicationDriver.h"
#include "Engine/World.h"
#include "Finder.h"
#include "GameplayTagContainer.h"
#include "Hooks.h"
#include "Items/FortInventory.h"
#include "Items/FortItem.h"
#include "Items/FortItemDefinition.h"
#include "Log.h"
#include "Natives.h"
#include "ObjectLayout.h"
#include "Pawns/FortPawn.h"
#include "Player/FortPlayerController.h"
#include "ReflectionParams.h"
#include "Templates/Casts.h"
#include "Weapons/FortWeapon.h"
#include "UObject/UObjectIterator.h"
#include "UObject/Class.h"
#include "UObject/WeakObjectPtr.h"

#include <algorithm>
#include <vector>

EFortQuickBars AFortQuickBars::GetCurrentQuickBar()
{
    for (EFortQuickBars InQuickBar : { EFortQuickBars::GetPrimary(), EFortQuickBars::GetSecondary() })
    {
        FQuickBar& QuickBar = GetQuickBar(InQuickBar);
        if (QuickBar.CurrentFocusedSlot != -1)
        {
            return InQuickBar;
        }
    }
    return EFortQuickBars::GetMax_None();
}

void AFortQuickBars::SwapQuickBarFocus(EFortQuickBars InQuickBar, int32_t SlotOverride)
{
    if (Natives::AFortQuickBars_SwapQuickBarFocus || Finder::ResolveQuickBarFocusLive())
    {
        Natives::AFortQuickBars_SwapQuickBarFocus(this, static_cast<int32_t>(InQuickBar), SlotOverride);
    }
}

int32_t AFortQuickBars::FocusSlot(EFortQuickBars InQuickBar, int32_t Slot, bool bUpdatePreviousFocusedSlot)
{
    if (Natives::AFortQuickBars_FocusSlot || Finder::ResolveQuickBarFocusLive())
    {
        return Natives::AFortQuickBars_FocusSlot(this, static_cast<int32_t>(InQuickBar), Slot, bUpdatePreviousFocusedSlot);
    }
    return -1;
}

bool AFortQuickBars::FocusItem(const FGuid& Item)
{
    for (EFortQuickBars InQuickBar : { EFortQuickBars::GetPrimary(), EFortQuickBars::GetSecondary() })
    {
        const int32_t NumSlots = GetQuickBar(InQuickBar).Slots.Num();
        for (int32_t Slot = 0; Slot < NumSlots; ++Slot)
        {
            if (GetSlot(InQuickBar, Slot).Items.Contains(Item))
            {
                return FocusSlot(InQuickBar, Slot, true) != -1;
            }
        }
    }
    return false;
}

bool AFortQuickBars::IsReplicatedToOwner()
{
    UWorld* World = GetWorld();
    UNetDriver* Driver = World ? World->GetNetDriver() : nullptr;
    return !Driver || !Driver->HasReplicationDriver() || !static_cast<UReplicationDriver*>(Driver->ReplicationDriver);
}

namespace
{
    // The client reports its own activation (ServerExecuteInventoryItem) a round trip after the swap's inventory change.
    constexpr float SwappedInItemLifetime = 2.0f;
    // How many of the client's executes after one swap are answered with the swapped-in item, so a client that keeps
    // going back to another stack can't make the two bounce for the whole lifetime.
    constexpr int32_t MaxSwappedInItemAnswers = 3;

    struct FSwappedInItem
    {
        TWeakObjectPtr<AFortQuickBars> QuickBars;
        FGuid Item;
        // Only compared, never read: the definition of the item the swap dropped.
        const UFortItemDefinition* SwappedOutDefinition;
        float Time;
        int32_t Answers;
        // The client executed the swapped-in item itself.
        bool bExecuted;
    };

    std::vector<FSwappedInItem> SwappedInItems;
}

void AFortQuickBars::SetSwappedInItem(const FGuid& Item, const UFortItemDefinition* SwappedOutDefinition)
{
    UWorld* World = GetWorld();
    std::erase_if(SwappedInItems, [this](const FSwappedInItem& Entry) { return !Entry.QuickBars.Get() || Entry.QuickBars.Get() == this; });
    SwappedInItems.push_back({ this, Item, SwappedOutDefinition, World ? World->GetTimeSeconds() : 0.0f, 0, false });
}

bool AFortQuickBars::GetSwappedInItemFor(const FGuid& ExecutedItem, const UFortItemDefinition* ExecutedDefinition, FGuid& OutItem)
{
    UWorld* World = GetWorld();
    const float Now = World ? World->GetTimeSeconds() : 0.0f;
    const auto It = std::find_if(SwappedInItems.begin(), SwappedInItems.end(), [this](const FSwappedInItem& Entry) { return Entry.QuickBars.Get() == this; });
    if (It == SwappedInItems.end())
    {
        return false;
    }
    if (Now - It->Time > SwappedInItemLifetime || It->Answers >= MaxSwappedInItemAnswers)
    {
        SwappedInItems.erase(It);
        return false;
    }
    if (ExecutedItem == It->Item)
    {
        // The client holds the swapped-in item. It is kept a while longer for a late move to the dropped item's other stack.
        It->bExecuted = true;
        return false;
    }
    // The client's first other item is where its quickbar moved when the held item went (#269). After that, only a stack of
    // the dropped item is answered (#309): with another stack of the held consumable in another slot, the client went on
    // to that stack (the full one) instead of the swapped-in weapon. Any other item is the player's own choice.
    if ((It->Answers > 0 || It->bExecuted) && (!ExecutedDefinition || ExecutedDefinition != It->SwappedOutDefinition))
    {
        SwappedInItems.erase(It);
        return false;
    }
    ++It->Answers;
    OutItem = It->Item;
    return true;
}

void AFortQuickBars::EnableSlot(EFortQuickBars InQuickBar, int32_t SlotIndex)
{
    // 3.0 made EnableSlot the server RPC ServerEnableSlot (same parameters); on the server it runs its implementation.
    UFunction* Function = FindStaticFunction_EnableSlot();
    Reflection::FParams Params(Function ? Function : StaticFunction_ServerEnableSlot());
    Params.Set(L"InQuickBar", static_cast<uint8_t>(InQuickBar));
    Params.Set(L"SlotIndex", SlotIndex);
    Params.Call(this);
}

void AFortQuickBars::ServerActivateSlotInternal(EFortQuickBars InQuickBar, int32_t Slot, float ActivateDelay, bool bUpdatePreviousFocusedSlot)
{
    Reflection::FParams Params(StaticFunction_ServerActivateSlotInternal());
    Params.Set(L"InQuickBar", static_cast<uint8_t>(InQuickBar));
    Params.Set(L"Slot", Slot);
    Params.Set(L"ActivateDelay", ActivateDelay);
    Params.Set(L"bUpdatePreviousFocusedSlot", bUpdatePreviousFocusedSlot);
    Params.Call(this);
}

void AFortQuickBars::ServerAddItemInternal(const FGuid& Item, EFortQuickBars InQuickBar, int32_t Slot)
{
    Reflection::FParams Params(StaticFunction_ServerAddItemInternal());
    Params.Set(L"Item", Item);
    Params.Set(L"InQuickBar", static_cast<uint8_t>(InQuickBar));
    Params.Set(L"Slot", Slot);
    Params.Call(this);
}

FQuickBarSlot& AFortQuickBars::GetSlot(EFortQuickBars InQuickBar, int32_t SlotIndex)
{
    TStructArray<FQuickBarSlot>& Slots = GetQuickBar(InQuickBar).Slots;
    return Slots[SlotIndex];
}

const FQuickBarSlotData& AFortQuickBars::GetSlotData(EFortQuickBars InQuickBar, int32_t SlotIndex)
{
    FQuickBarData& DataDefinition = GetQuickBar(InQuickBar).DataDefinition;
    TStructArray<FQuickBarSlotData>& SlotData = DataDefinition.QuickbarSlots;
    return SlotData[SlotIndex];
}

bool AFortQuickBars::ContainsItem(const FGuid& Item)
{
    for (EFortQuickBars Bar : { EFortQuickBars::GetPrimary(), EFortQuickBars::GetSecondary() })
    {
        const int32_t NumSlots = GetQuickBar(Bar).Slots.Num();
        for (int32_t SlotIndex = 0; SlotIndex < NumSlots; ++SlotIndex)
        {
            if (GetSlot(Bar, SlotIndex).Items.Contains(Item))
            {
                return true;
            }
        }
    }
    return false;
}

void AFortQuickBars::ServerRemoveItemInternal(const FGuid& Item, bool bFindReplacement, bool bForce)
{
    Reflection::FParams Params(StaticFunction_ServerRemoveItemInternal());
    Params.Set(L"Item", Item);
    Params.Set(L"bFindReplacement", bFindReplacement);
    Params.Set(L"bForce", bForce);
    Params.Call(this);
}

void AFortQuickBars::ServerRemoveItemInternal_Implementation(const FGuid& Item, bool bFindReplacement, bool bForce)
{
#if WITH_SERVER_CODE
    bool bRemovedFocusedItem = false;
    EFortQuickBars FocusedBar = EFortQuickBars::GetPrimary();
    for (EFortQuickBars Bar : { EFortQuickBars::GetPrimary(), EFortQuickBars::GetSecondary() })
    {
        FQuickBar& QuickBar = GetQuickBar(Bar);
        const int32_t NumSlots = QuickBar.Slots.Num();
        for (int32_t SlotIndex = 0; SlotIndex < NumSlots; ++SlotIndex)
        {
            TArray<FGuid>& Items = GetSlot(Bar, SlotIndex).Items;
            for (int32_t Index = Items.Num() - 1; Index >= 0; --Index)
            {
                if (Items[Index] == Item)
                {
                    Items.RemoveAt(Index);
                    if (SlotIndex == QuickBar.CurrentFocusedSlot && Items.Num() == 0)
                    {
                        bRemovedFocusedItem = true;
                        FocusedBar = Bar;
                    }
                }
            }
        }
    }

    AFortPlayerController* Controller = Cast<AFortPlayerController>(GetOwner());
    AFortPawn* Pawn = Controller ? static_cast<AFortPawn*>(Controller->MyFortPawn) : nullptr;
    AFortWeapon* CurrentWeapon = Pawn ? static_cast<AFortWeapon*>(Pawn->CurrentWeapon) : nullptr;
    const bool bRemovedHeldItem = CurrentWeapon && static_cast<FGuid&>(CurrentWeapon->ItemEntryGuid) == Item;
    const bool bWeaponReactivatesSlot = bRemovedHeldItem && CurrentWeapon->IsPendingDestroyDueToDurability();
    // The held weapon's own item went while its pawn is alive: a consumable used up by its own ability, or the held item
    // dropped. The game then destroys the weapon (below), and only another item's equip takes it out of the pawn's hands.
    const bool bHeldItemReplaced = bRemovedHeldItem && !Pawn->IsDead();
    if (bRemovedFocusedItem && KeepFocusOnRemoveCount > 0)
    {
        Log::Info(L"Quickbar: {} keeps slot {} focused while its item is replaced", GetNameSafe(Controller).ToStdString(), static_cast<int32_t>(GetQuickBar(FocusedBar).CurrentFocusedSlot));
    }
    else if (bRemovedFocusedItem && bWeaponReactivatesSlot)
    {
        FQuickBar& QuickBar = GetQuickBar(FocusedBar);
        const int32_t NumSlots = QuickBar.Slots.Num();
        const int32_t EmptiedSlot = QuickBar.CurrentFocusedSlot;
        for (int32_t Step = 1; Step < NumSlots; ++Step)
        {
            if (FocusSlot(FocusedBar, (EmptiedSlot + Step) % NumSlots, true) != -1)
            {
                break;
            }
        }
    }
    else if (bRemovedFocusedItem && !IsReplicatedToOwner() && !bHeldItemReplaced)
    {
        // The client's own quickbar moves its focus on to the next slot and activates it (3.5.2 0xA75070 -> 0xD03200).
        // An activation from here would send it a second item to equip.
        Log::Info(L"Quickbar: {}'s client picks the slot after its focused item was removed", GetNameSafe(Controller).ToStdString());
    }
    else if (bRemovedFocusedItem)
    {
        // As AFortQuickBarsAthena's own ServerRemoveItemInternal_Implementation (5.10 0xC5B3E0) does for its held item: the
        // focus cycles on to the next slot (CycleSlot(Delay, 1), 0xF59860), which activates there. Its activation is
        // delayed by 0.1s while the player can't change equipment (CanPerformAction(Gameplay.Action.Player.ChangeEquipment)
        // false), as the execute it sends would be dropped then (AFortPlayerController::ExecuteInventoryItem, 0x1142880,
        // "blocked ActionPlayerChangeEquipment tag"). Save the World's inventory screen sends this remove when the
        // equipped weapon goes back to the backpack (UFortInventoryContext::RemoveItemFromQuickBar, 0x2CD86A0); the
        // previous slot or the harvesting tool's activated instead left the weapon in the player's hand (#379).
        //
        // Where the client has a quickbar of its own (3.4+ Battle Royale) this runs for the held item as well (#413,
        // #419). Once that item has left the inventory, the game marks the weapon bPendingDestroyDueToDurabilityOrStackCount
        // (its inventory update handler, 6.01 0x1362B70); when its ability ends it activates the server's focused slot 0.1s
        // later and gives the weapon an 8s lifespan (AFortPawn's ability-ended handler 0x11A74A0 -> 0x1340D90). Nothing
        // takes a destroyed weapon out of the pawn's hands or its dependent actors: only AFortPawn::EquipWeapon (0x11A2E70)
        // does, through the static weapon-changed delegate (0x4E35460) UFortReplicationGraph's handler (0x111FC90) is
        // bound to, which swaps the old weapon for the new one in the pawn's dependent actor list. The
        // Rift-To-Go's portal starts GA_Rift_Athena_Skydive in the frame of its cost, and Gameplay.Action.Player.Skydive
        // blocks ChangeEquipment until the player lands (AbilityTagRelationshipMapping): the client's own move, which only
        // starts when the inventory change reaches it, and the game's 0.1s activation were both dropped, the jar was
        // destroyed in the player's hands, and the replication graph opened a channel on it again as the pawn's dependent.
        // UActorChannel::ReplicateActor freed that channel's replicator for the pending kill actor and the next frame read
        // it (FObjectReplicator::ReplicateProperties, 6.01 0x2507D00). The activation from here goes out with the cost,
        // ahead of the skydive (ServerActivateSlotInternal -> ExecuteInventoryItem 0x11DC3E0 -> ClientExecuteInventoryItem),
        // and the client's ServerExecuteInventoryItem equips the slot's item (UFortWeaponItemDefinition::ServerExecute,
        // 0x1368680, asks no tag). It is the slot the client's quickbar moves to as well, so its own execute changes nothing.
        float ActivateDelay = 0.0f;
        if (Controller && Natives::AFortPlayerController_CanPerformActionIndex >= 0)
        {
            const FGameplayTag ChangeEquipment{ FName(L"Gameplay.Action.Player.ChangeEquipment") };
            if (!CallVirtual<bool>(Controller, Natives::AFortPlayerController_CanPerformActionIndex, &ChangeEquipment))
            {
                ActivateDelay = 0.1f;
            }
        }
        CycleSlot(ActivateDelay, 1);
    }
#endif
}

void AFortQuickBars::CycleSlot(float ActivateDelay, int32_t Direction)
{
#if WITH_SERVER_CODE
    const EFortQuickBars Bar = GetCurrentQuickBar();
    if (Bar == EFortQuickBars::GetMax_None())
    {
        return;
    }
    FQuickBar& QuickBar = GetQuickBar(Bar);
    const int32_t NumSlots = QuickBar.Slots.Num();
    const int32_t CurrentSlot = QuickBar.CurrentFocusedSlot;
    if (NumSlots <= 0)
    {
        return;
    }

    // The first slot on from the focused one that FocusSlot takes, all the way round to it at most.
    int32_t Slot = -1;
    for (int32_t Step = 1; Step <= NumSlots; ++Step)
    {
        const int32_t Candidate = ((CurrentSlot + Step * Direction) % NumSlots + NumSlots) % NumSlots;
        if (Candidate == CurrentSlot)
        {
            return;
        }
        if (FocusSlot(Bar, Candidate, true) != -1)
        {
            Slot = Candidate;
            break;
        }
    }
    if (Slot < 0)
    {
        return;
    }

    // The game activates the slot only for an item that activates on focus (bShouldActivateWhenFocused); for any other,
    // its Save the World controllers unequip the pawn's weapon instead (0xF59A7E), which Core doesn't do.
    AFortPlayerController* Controller = Cast<AFortPlayerController>(GetOwner());
    const TArray<FGuid>& Items = GetSlot(Bar, Slot).Items;
    AFortInventory* Inventory = Controller ? static_cast<AFortInventory*>(Controller->WorldInventory) : nullptr;
    UFortWorldItem* WorldItem = Inventory && Items.Num() > 0 ? Inventory->FindWorldItem(Items[0]) : nullptr;
    FFortItemEntry* ItemEntry = WorldItem ? &static_cast<FFortItemEntry&>(WorldItem->ItemEntry) : nullptr;
    const UFortWorldItemDefinition* ItemDefinition = ItemEntry ? Cast<UFortWorldItemDefinition>(static_cast<UFortItemDefinition*>(ItemEntry->ItemDefinition)) : nullptr;
    if (ItemDefinition && ItemDefinition->ShouldActivateWhenFocused())
    {
        Log::Info(L"Quickbar: {} activates slot {} after its focused item was removed{}", GetNameSafe(Controller).ToStdString(), Slot, ActivateDelay > 0.0f ? L" (delayed, equipment change blocked)" : L"");
        ServerActivateSlotInternal(Bar, Slot, ActivateDelay, true);
    }
    else
    {
        Log::Info(L"Quickbar: {} focuses slot {} after its focused item was removed; its item doesn't activate on focus", GetNameSafe(Controller).ToStdString(), Slot);
    }
#endif
}

void AFortQuickBars::ActivateReplacementSlot()
{
#if WITH_SERVER_CODE
    // The primary bar's previously focused slot if it still holds an item, otherwise the harvesting tool's (slot 0).
    FQuickBar& PrimaryBar = PrimaryQuickBar;
    const int32_t PreviousSlot = PrimaryBar.PreviousFocusedSlot;
    const bool bUsePrevious = PreviousSlot >= 0 && PreviousSlot < PrimaryBar.Slots.Num() && PreviousSlot != PrimaryBar.CurrentFocusedSlot && GetSlot(EFortQuickBars::GetPrimary(), PreviousSlot).Items.Num() > 0;
    const int32_t Slot = bUsePrevious ? PreviousSlot : 0;
    Log::Info(L"Quickbar: {} activates slot {} after its focused item was removed", GetNameSafe(GetOwner()).ToStdString(), Slot);
    ServerActivateSlotInternal(EFortQuickBars::GetPrimary(), Slot, 0.0f, false);
#endif
}

void AFortQuickBars::HookVTables()
{
    const int32_t RemoveIndex = Finder::ImplementationIndex(StaticFunction_ServerRemoveItemInternal());
    if (RemoveIndex < 0)
    {
        Log::Error(L"Finder: AFortQuickBars::ServerRemoveItemInternal_Implementation slot not found");
        return;
    }

    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Class = *It;
        UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
        void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
        if (VTable && VTable[RemoveIndex] == Natives::SharedEmptyFunction)
        {
            Hooks::SwapVTable(L"AFortQuickBars::ServerRemoveItemInternal_Implementation", VTable, RemoveIndex, reinterpret_cast<void*>(&ServerRemoveItemInternal_ImplementationHK));
        }
    }
}

void AFortQuickBars::ServerRemoveItemInternal_ImplementationHK(AFortQuickBars* This, const FGuid& Item, bool bFindReplacement, bool bForce)
{
    This->ServerRemoveItemInternal_Implementation(Item, bFindReplacement, bForce);
}

void AFortQuickBars::SetupQuickbar(EFortQuickBars InQuickBar, const FQuickBarData& QuickBarData)
{
#if WITH_SERVER_CODE
    FQuickBar& QuickBar = GetQuickBar(InQuickBar);
    FQuickBarData& DataDefinition = QuickBar.DataDefinition;
    FQuickBarData::StaticStruct()->CopyScriptStruct(&DataDefinition, &QuickBarData);

    TStructArray<FQuickBarSlot>& Slots = QuickBar.Slots;
    const int32_t NumSlots = DataDefinition.QuickbarSlots.Num();
    while (Slots.Num() < NumSlots)
    {
        Slots.AddDefaulted();
    }

    for (int32_t SlotIndex = 0; SlotIndex < NumSlots; ++SlotIndex)
    {
        EnableSlot(InQuickBar, SlotIndex);
    }
#endif
}
