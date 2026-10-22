#pragma once

#include "Misc/Guid.h"
#include "Pawns/FortPawn.h"

class IFortInventoryOwnerInterface;

class AFortAIPawn : public AFortPawn
{
    DefineUnrealClass(AFortAIPawn, FortniteGame)

public:
    IFortInventoryOwnerInterface* GetInventoryOwner();
    bool RemoveInventoryItem(const FGuid& ItemGuid, int32_t Count, bool bForceRemoveFromQuickBars);

    static void HookVTables();

private:
    static bool RemoveInventoryItemHK(void* InventoryOwner, const FGuid& ItemGuid, int32_t Count, bool bForceRemoveFromQuickBars);

    static inline ptrdiff_t InventoryOwnerInterfaceOffset = -1;
};
