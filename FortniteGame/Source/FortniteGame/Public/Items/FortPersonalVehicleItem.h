#pragma once

#include "Items/FortItem.h"

// The profile0 instance of a UFortPersonalVehicleItemDefinition (Epic's derives from UFortAccountItem), in the player's
// MainMcpProfile.
class UFortPersonalVehicleItem : public UFortItem
{
    DefineUnrealClass(UFortPersonalVehicleItem, FortniteGame)
};
