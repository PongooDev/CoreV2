#pragma once

#include "GameFramework/Actor.h"
#include "Misc/Guid.h"

class UFortWeaponItemDefinition;

class AFortWeapon : public AActor
{
    DefineUnrealClass(AFortWeapon, FortniteGame)
    DefineUFunction(GetBulletsPerClip)
    DefineUFunction(GetLocalMagazineAmmoCount)

public:
    DefineUProperty(UFortWeaponItemDefinition*, WeaponData)
    DefineUProperty(FGuid, ItemEntryGuid)
    DefineUProperty(int32_t, AmmoCount)
    DefineBitfieldUProperty(bPendingDestroyDueToDurability)
    DefineBitfieldUProperty(bPendingDestroyDueToDurabilityOrStackCount)

    int32_t GetBulletsPerClip() const;
    // The magazine as this machine has it: a client's own count, which its shots take from before the server's AmmoCount
    // comes back (and only replaces it once one of the two is empty). -1 when the build has no such function.
    int32_t GetLocalMagazineAmmoCount() const;
    void ModAmmo(int32_t AmmoDelta);
    void DestroyedDueToDurability();
    bool IsPendingDestroyDueToDurability() const;

    static ptrdiff_t PendingDestroyDueToDurabilityOffset();
    static uint8_t PendingDestroyDueToDurabilityMask();

private:
    static Reflection::FMemberInfo& PendingDestroyDueToDurabilityMember();
};

class ABuildingSMActor;

class AFortWeap_EditingTool : public AFortWeapon
{
    DefineUnrealClass(AFortWeap_EditingTool, FortniteGame)
    DefineUFunction(OnRep_EditActor)

public:
    DefineUProperty(ABuildingSMActor*, EditActor)

    void SetEditActor(ABuildingSMActor* NewEditActor);
};

class AFortWeaponRanged : public AFortWeapon
{
    DefineUnrealClass(AFortWeaponRanged, FortniteGame)
};
