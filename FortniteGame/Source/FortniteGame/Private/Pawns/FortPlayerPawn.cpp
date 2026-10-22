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
    return CallVirtual<bool>(this, Natives::AFortPlayerPawn_EquipLastWeaponIndex);
}

ptrdiff_t AFortPlayerPawn::LastEquippedWeaponGUIDOffset()
{
    return Member_LastEquippedWeaponGUID.GetOffsetChecked();
}
