#pragma once

#include "Misc/Guid.h"
#include "Natives.h"
#include "ObjectLayout.h"
#include "UObject/Object.h"

class AFortInventory;
class AFortPickup;

class UFortInventoryOwnerInterface : public UObject
{
    DefineUnrealClass(UFortInventoryOwnerInterface, FortniteGame)
};

class IFortInventoryOwnerInterface
{
public:
    virtual ~IFortInventoryOwnerInterface() = default;
    virtual UObject* _getUObject() const = 0;

    AFortInventory* GetWorldInventory()
    {
        return CallVirtual<AFortInventory*>(this, Natives::IFortInventoryOwnerInterface_GetWorldInventoryIndex);
    }

    // Before and after a pickup's items are given (AFortPickup::GivePickupTo). The player controller's pass the pickup to
    // the game state's OnPickupPreGivenToPlayer / OnPickupPostGivenToPlayer, which ask its mutators (8.50's Endgame captures
    // a stone there). Only where Finder::ResolvePickupGivenLive found the slots.
    static bool HasOnPickupGiven() { return Natives::IFortInventoryOwnerInterface_OnPickupPreGivenIndex >= 0; }
    void OnPickupPreGiven(AFortPickup* Pickup)
    {
        CallVirtual<void>(this, Natives::IFortInventoryOwnerInterface_OnPickupPreGivenIndex, Pickup);
    }
    void OnPickupPostGiven(AFortPickup* Pickup)
    {
        CallVirtual<void>(this, Natives::IFortInventoryOwnerInterface_OnPickupPostGivenIndex, Pickup);
    }
};
