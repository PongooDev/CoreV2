#pragma once

#include "Math/Vector.h"
#include "Reflection.h"
#include "Weapons/FortWeapon.h"

// The Pirate Cannon's weapon (AFortAthenaSKPushCannon's CachedWeapon). Its fire on the pushing player's client
// (8.30 0x17EC830) aims the barrel and, with a passenger in seat 1, sends ServerFireActorInCannon with the launch
// direction.
class AFortWeaponRangedMountedCannon : public AFortWeaponRanged
{
    DefineUnrealClass(AFortWeaponRangedMountedCannon, FortniteGame)
    DefineUFunction(ServerFireActorInCannon)

public:
    // Restored (8.30 strips it, see the .cpp): the passenger in seat 1 leaves the cannon and is launched along LaunchDir.
    void ServerFireActorInCannon_Implementation(const FVector& LaunchDir);

    static void HookVTables();

private:
    static void ServerFireActorInCannon_ImplementationHK(AFortWeaponRangedMountedCannon* This, const FVector& LaunchDir);
};
