#pragma once

#include "GameFramework/Pawn.h"
#include "Misc/Guid.h"

class AFortWeapon;
class UFortWeaponItemDefinition;

class AFortPawn : public APawn
{
    DefineUnrealClass(AFortPawn, FortniteGame)
    DefineUFunction(EquipWeaponDefinition)

public:
    AFortWeapon* EquipWeaponDefinition(const UFortWeaponItemDefinition* WeaponData, const FGuid& ItemEntryGuid);
};
