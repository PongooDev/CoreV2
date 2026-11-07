#pragma once

#include "Containers/Array.h"
#include "StructArray.h"
#include "GameFramework/Actor.h"
#include "Items/FortItemDefinition.h"
#include "Misc/Guid.h"

class EFortQuickBars
{
    DefineUnrealEnum(EFortQuickBars, FortniteGame)
    DefineUEnumValue(Primary)
    DefineUEnumValue(Secondary)
    DefineUEnumValue(Max_None)
};

struct FQuickBarSlot
{
    DefineUnrealStruct(FQuickBarSlot, FortniteGame)
    DefineUProperty(TArray<FGuid>, Items)
    DefineUProperty(bool, bEnabled)
};

struct FQuickBarSlotData
{
    DefineUnrealStruct(FQuickBarSlotData, FortniteGame)
    DefineUProperty(TArray<EFortItemType>, AcceptedItemTypes)
};

struct FQuickBarData
{
    DefineUnrealStruct(FQuickBarData, FortniteGame)
    DefineUStructArrayProperty(FQuickBarSlotData, QuickbarSlots)
};

struct FQuickBar
{
    DefineUnrealStruct(FQuickBar, FortniteGame)
    DefineUProperty(int32_t, CurrentFocusedSlot)
    DefineUProperty(int32_t, PreviousFocusedSlot)
    DefineUStructArrayProperty(FQuickBarSlot, Slots)
    DefineUStructProperty(FQuickBarData, DataDefinition)
};

class AFortQuickBars : public AActor
{
    DefineUnrealClass(AFortQuickBars, FortniteGame)

public:
    DefineUStructProperty(FQuickBar, PrimaryQuickBar)
    DefineUStructProperty(FQuickBar, SecondaryQuickBar)
    DefineUFunction(EnableSlot)
    DefineUFunction(ServerEnableSlot)
    DefineUFunction(ServerActivateSlotInternal)
    DefineUFunction(ServerAddItemInternal)
    DefineUFunction(ServerRemoveItemInternal)

    // While one is alive, removing the item of the focused slot leaves the focus on that slot instead of activating
    // another one (ServerRemoveItemInternal_Implementation), for a slot that is filled again straight away.
    struct FKeepFocusOnRemove
    {
        FKeepFocusOnRemove() { ++KeepFocusOnRemoveCount; }
        ~FKeepFocusOnRemove() { --KeepFocusOnRemoveCount; }
        FKeepFocusOnRemove(const FKeepFocusOnRemove&) = delete;
        FKeepFocusOnRemove& operator=(const FKeepFocusOnRemove&) = delete;
    };

    FQuickBar& GetQuickBar(EFortQuickBars InQuickBar) { return InQuickBar == EFortQuickBars::GetPrimary() ? PrimaryQuickBar : SecondaryQuickBar; }
    EFortQuickBars GetCurrentQuickBar();
    void SwapQuickBarFocus(EFortQuickBars InQuickBar, int32_t SlotOverride);
    int32_t FocusSlot(EFortQuickBars InQuickBar, int32_t Slot, bool bUpdatePreviousFocusedSlot);
    // Focuses the slot that holds the item, on the primary bar or else the secondary one (3.5.2 0xD0D890). The client's
    // ClientExecuteInventoryItem_Implementation (0xEB8F50) focuses an item it executes this way before it sends
    // ServerExecuteInventoryItem. Returns whether such a slot was focused.
    bool FocusItem(const FGuid& Item);

    // Whether the owner's client has this quickbar. 3.4's Battle Royale replication graph doesn't (3.5.2
    // UFortReplicationGraph's class routing 0xE44470 leaves AFortQuickBars NotRouted, and the per-connection node 0xE38900
    // adds only the controller, its player state, its WorldInventory and the pawn with its weapons). So 3.5's client
    // spawns its own AFortQuickBarsAthena (AFortPlayerControllerAthena's OnReadyToStartMatch, 0xA2BAC0, "Quick Bars
    // failed to spawn!", which 3.3 doesn't have). Its slot activation (0xA45D80) calls ServerActivateSlotInternal_Implementation
    // directly, so the server's quickbar never sees the player switch slots. The server learns of it only from
    // ServerExecuteInventoryItem.
    bool IsReplicatedToOwner();

    void EnableSlot(EFortQuickBars InQuickBar, int32_t SlotIndex);
    void ServerActivateSlotInternal(EFortQuickBars InQuickBar, int32_t Slot, float ActivateDelay, bool bUpdatePreviousFocusedSlot);
    void ServerAddItemInternal(const FGuid& Item, EFortQuickBars InQuickBar, int32_t Slot);
    FQuickBarSlot& GetSlot(EFortQuickBars InQuickBar, int32_t SlotIndex);
    const FQuickBarSlotData& GetSlotData(EFortQuickBars InQuickBar, int32_t SlotIndex);
    bool ContainsItem(const FGuid& Item);
    void ServerRemoveItemInternal(const FGuid& Item, bool bFindReplacement, bool bForce);
    void ServerRemoveItemInternal_Implementation(const FGuid& Item, bool bFindReplacement, bool bForce);
    void ActivateReplacementSlot();
    void SetupQuickbar(EFortQuickBars InQuickBar, const FQuickBarData& QuickBarData);

    // A weapon swapped in for the held one, on a quickbar the client doesn't have (#269). The client's own quickbar moves
    // its focus to the next slot when the held item goes (AFortQuickBarsAthena's ServerRemoveItemInternal_Implementation,
    // 3.5.2 0xA75070, cycles on the primary bar). That activation goes through ClientExecuteInventoryItem_Implementation,
    // which replaces the item the server's execute left pending, so the server asks again once the client reports it.
    // SwappedOutDefinition is the dropped item's definition: with another stack of it elsewhere on the quickbar (#309), the
    // client can move on to that stack even after it took the swapped-in item.
    void SetSwappedInItem(const FGuid& Item, const UFortItemDefinition* SwappedOutDefinition);
    // Whether the client's execute of ExecutedItem is to be answered by executing the swapped-in item again (OutItem): its
    // first execute of another item, and after that one of the dropped item's other stacks. Only for a short while after
    // the swap, and a few times at most.
    bool GetSwappedInItemFor(const FGuid& ExecutedItem, const UFortItemDefinition* ExecutedDefinition, FGuid& OutItem);

    static void HookVTables();

private:
    static inline int32_t KeepFocusOnRemoveCount = 0;

    static void ServerRemoveItemInternal_ImplementationHK(AFortQuickBars* This, const FGuid& Item, bool bFindReplacement, bool bForce);
};
