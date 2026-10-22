#pragma once

#include "Containers/Array.h"
#include "GameFramework/Pawn.h"
#include "Misc/Guid.h"

class AFortWeapon;
class UFortWeaponItemDefinition;

class AFortPawn : public APawn
{
    DefineUnrealClass(AFortPawn, FortniteGame)
    DefineUFunction(IsDead)
    DefineUFunction(EquipWeaponDefinition)

public:
    DefineUProperty(AFortWeapon*, CurrentWeapon)
    DefineUProperty(TArray<AFortWeapon*>, CurrentWeaponList)

    bool IsDead() const;
    AFortWeapon* EquipWeaponDefinition(const UFortWeaponItemDefinition* WeaponData, const FGuid& ItemEntryGuid);
    bool EquipDefaultWeapon();
    void RemoveAbilityWeapon(AFortWeapon* Weapon);

    static ptrdiff_t CurrentWeaponListOffset();
};
