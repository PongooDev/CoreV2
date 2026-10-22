#pragma once

#include "Pawns/FortPawn.h"

class AFortPlayerPawn : public AFortPawn
{
    DefineUnrealClass(AFortPlayerPawn, FortniteGame)

public:
    DefineUProperty(FGuid, LastEquippedWeaponGUID)

    void ResetGameplayAttributes();
    bool EquipLastWeapon();

    static ptrdiff_t LastEquippedWeaponGUIDOffset();
};
