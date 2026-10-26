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
    DefineBitfieldUProperty(bPendingDestroyDueToDurabilityOrStackCount)

    int32_t GetBulletsPerClip() const;
    void ModAmmo(int32_t AmmoDelta);
    void DestroyedDueToDurability();
    bool IsPendingDestroyDueToDurability() const;

    static ptrdiff_t PendingDestroyDueToDurabilityOffset();
    static uint8_t PendingDestroyDueToDurabilityMask();

private:
    static Reflection::FMemberInfo& PendingDestroyDueToDurabilityMember();
};

class AFortWeaponRanged : public AFortWeapon
{
    DefineUnrealClass(AFortWeaponRanged, FortniteGame)
};
