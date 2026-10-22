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
    DefineBitfieldUProperty(bPendingDestroyDueToDurability)

    int32_t GetBulletsPerClip() const;
    void ModAmmo(int32_t AmmoDelta);
    void DestroyedDueToDurability();

    static ptrdiff_t PendingDestroyDueToDurabilityOffset();
    static uint8_t PendingDestroyDueToDurabilityMask();
};
