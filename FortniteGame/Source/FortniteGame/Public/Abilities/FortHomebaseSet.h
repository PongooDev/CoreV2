#pragma once

#include "AttributeSet.h"

// The player's homebase stats. Its WorldInventorySizeBonus is the backpack's size (AFortInventory 0x63CA40).
class UFortHomebaseSet : public UAttributeSet
{
    DefineUnrealClass(UFortHomebaseSet, FortniteGame)
};
