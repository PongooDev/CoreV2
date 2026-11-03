#pragma once

#include "Misc/Guid.h"
#include "Pawns/FortPawn.h"

class ABuildingTrapDefender;
class AFortInventory;
class AFortPlayerController;
class AFortPlayerStateZone;
class IFortInventoryOwnerInterface;
class UFortWorldItem;

class AFortAIPawn : public AFortPawn
{
    DefineUnrealClass(AFortAIPawn, FortniteGame)

public:
    // A defender's: the player who placed it (its post's summoner) and the post it stands at.
    DefineUProperty(AFortPlayerStateZone*, PlayerManager)
    DefineUProperty(ABuildingTrapDefender*, DefenderTrap)

    IFortInventoryOwnerInterface* GetInventoryOwner();
    AFortInventory* GetWorldInventory();
    bool RemoveInventoryItem(const FGuid& ItemGuid, int32_t Count, bool bForceRemoveFromQuickBars);

    // The controller of the player who manages this defender (PlayerManager's owner), null once they have left.
    AFortPlayerController* GetPlayerManagerController();
    // A defender leaving its post gives its weapons and ammo back to the player who manages it; with that player gone,
    // they drop where it stands.
    void ReturnInventoryToPlayerManager();
    // Count of Item (one in this pawn's inventory) to that player, or to the ground; returns the player's item it became.
    UFortWorldItem* ReturnItemToPlayerManager(UFortWorldItem* Item, int32_t Count);

    static void HookVTables();

private:
    static bool RemoveInventoryItemHK(void* InventoryOwner, const FGuid& ItemGuid, int32_t Count, bool bForceRemoveFromQuickBars);

    static inline ptrdiff_t InventoryOwnerInterfaceOffset = -1;
};
