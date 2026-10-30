#include "pch.h"
#include "Pawns/FortAIPawn.h"
#include "Finder.h"
#include "Hooks.h"
#include "Items/FortInventory.h"
#include "Items/FortInventoryOwnerInterface.h"
#include "Log.h"
#include "Natives.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"

IFortInventoryOwnerInterface* AFortAIPawn::GetInventoryOwner()
{
    return InventoryOwnerInterfaceOffset >= 0 ? reinterpret_cast<IFortInventoryOwnerInterface*>(reinterpret_cast<uint8_t*>(this) + InventoryOwnerInterfaceOffset) : nullptr;
}

bool AFortAIPawn::RemoveInventoryItem(const FGuid& ItemGuid, int32_t Count, bool bForceRemoveFromQuickBars)
{
#if WITH_SERVER_CODE
    IFortInventoryOwnerInterface* InventoryOwner = GetInventoryOwner();
    AFortInventory* Inventory = InventoryOwner && Natives::IFortInventoryOwnerInterface_GetWorldInventoryIndex >= 0 ? InventoryOwner->GetWorldInventory() : nullptr;
    UFortWorldItem* WorldItem = Role == ROLE_Authority && Inventory ? Inventory->FindWorldItem(ItemGuid) : nullptr;
    if (!WorldItem)
    {
        return false;
    }

    FFortItemEntry& ItemEntry = WorldItem->ItemEntry;
    const int32_t Remaining = ItemEntry.Count - Count;
    if (Count < 0 || Remaining <= 0)
    {
        Inventory->RemoveWorldItem(WorldItem);
    }
    else
    {
        Inventory->UpdateWorldItemCount(WorldItem, Remaining);
    }
    return true;
#else
    return false;
#endif
}

void AFortAIPawn::HookVTables()
{
    UObject* PawnDefault = StaticClass()->GetDefaultObject(false);
    void* InventoryOwner = PawnDefault && Natives::UObject_GetInterfaceAddress ? Natives::UObject_GetInterfaceAddress(PawnDefault, UFortInventoryOwnerInterface::StaticClass()) : nullptr;
    const int32_t RemoveIndex = Natives::IFortInventoryOwnerInterface_RemoveInventoryItemIndex;
    if (!InventoryOwner || RemoveIndex < 0)
    {
        Log::Error(L"Finder: AFortAIPawn's IFortInventoryOwnerInterface not found");
        return;
    }

    InventoryOwnerInterfaceOffset = reinterpret_cast<uint8_t*>(InventoryOwner) - reinterpret_cast<uint8_t*>(PawnDefault);
    const uint8_t* PawnRemove = static_cast<const uint8_t*>((*reinterpret_cast<void***>(InventoryOwner))[RemoveIndex]);
    // The stripped stub is `xor al, al; ret`; on an exe with moved code (1.9.1) its return is rewritten.
    if (!Finder::BodyIs(reinterpret_cast<uintptr_t>(PawnRemove), { 0x32, 0xC0, 0xC3 }))
    {
        Log::Error(L"Finder: AFortAIPawn::RemoveInventoryItem is not the stripped stub");
        return;
    }

    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Class = *It;
        UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
        void** InterfaceVTable = ClassDefault ? *reinterpret_cast<void***>(reinterpret_cast<uint8_t*>(ClassDefault) + InventoryOwnerInterfaceOffset) : nullptr;
        if (InterfaceVTable && InterfaceVTable[RemoveIndex] == PawnRemove)
        {
            Hooks::SwapVTable(L"AFortAIPawn::RemoveInventoryItem", InterfaceVTable, RemoveIndex, reinterpret_cast<void*>(&RemoveInventoryItemHK));
        }
    }
}

bool AFortAIPawn::RemoveInventoryItemHK(void* InventoryOwner, const FGuid& ItemGuid, int32_t Count, bool bForceRemoveFromQuickBars)
{
    AFortAIPawn* Pawn = reinterpret_cast<AFortAIPawn*>(static_cast<uint8_t*>(InventoryOwner) - InventoryOwnerInterfaceOffset);
    return Pawn->RemoveInventoryItem(ItemGuid, Count, bForceRemoveFromQuickBars);
}
