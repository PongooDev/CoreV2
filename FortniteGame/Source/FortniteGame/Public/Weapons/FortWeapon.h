#pragma once

#include "GameFramework/Actor.h"
#include "Misc/Guid.h"

class UFortWeaponItemDefinition;

class AFortWeapon : public AActor
{
    DefineUnrealClass(AFortWeapon, FortniteGame)
    DefineUFunction(GetBulletsPerClip)

public:
    DefineUProperty(UFortWeaponItemDefinition*, WeaponData)
    DefineUProperty(FGuid, ItemEntryGuid)
    DefineUProperty(int32_t, AmmoCount)

    int32_t GetBulletsPerClip() const;
    void ModAmmo(int32_t AmmoDelta);
};
