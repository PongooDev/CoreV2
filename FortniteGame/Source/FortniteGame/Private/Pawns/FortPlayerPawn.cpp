#include "pch.h"
#include "Pawns/FortPlayerPawn.h"
#include "Finder.h"
#include "Natives.h"
#include "ObjectLayout.h"

void AFortPlayerPawn::ResetGameplayAttributes()
{
    if (Finder::ResolveResetGameplayAttributesLive())
    {
        CallVirtual<void>(this, Natives::AFortPlayerPawn_ResetGameplayAttributesIndex);
    }
}

bool AFortPlayerPawn::EquipLastWeapon()
{
    if (!Finder::ResolveAbilityWeaponLive(CurrentWeaponListOffset(), LastEquippedWeaponGUIDOffset()))
    {
        return false;
    }
    // 1.7.2 added a bool the game's own callers pass as false; 1.1 and 1.2 ignore it.
    return CallVirtual<bool>(this, Natives::AFortPlayerPawn_EquipLastWeaponIndex, false);
}

ptrdiff_t AFortPlayerPawn::LastEquippedWeaponGUIDOffset()
{
    return Member_LastEquippedWeaponGUID.GetOffsetChecked();
}
