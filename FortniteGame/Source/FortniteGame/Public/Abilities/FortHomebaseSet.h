#pragma once

#include "Abilities/FortPlayerAttrSet.h"

// The player's homebase stats. Its WorldInventorySizeBonus is the backpack's size (AFortInventory 0x63CA40), its
// StorageInventorySizeBonus the storm shield storage's (UFortOutpostFunctionLibrary::GetStorageVaultCapacity, 2.4 0x7E7BD0).
class UFortHomebaseSet : public UAttributeSet
{
    DefineUnrealClass(UFortHomebaseSet, FortniteGame)

public:
    DefineUStructProperty(FFortGameplayAttributeData, StorageInventorySizeBonus)
};
