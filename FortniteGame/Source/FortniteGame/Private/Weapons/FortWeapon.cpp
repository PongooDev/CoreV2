#include "pch.h"
#include "Weapons/FortWeapon.h"
#include "Finder.h"
#include "Natives.h"
#include "ObjectLayout.h"
#include "ReflectionParams.h"
#include "UObject/Class.h"

int32_t AFortWeapon::GetBulletsPerClip() const
{
    Reflection::FParams Params(StaticFunction_GetBulletsPerClip());
    return Params.Call(const_cast<AFortWeapon*>(this)) ? Params.Take<int32_t>(L"ReturnValue") : 0;
}

void AFortWeapon::ModAmmo(int32_t AmmoDelta)
{
    if (Finder::ResolveWeaponAmmoLive(Member_AmmoCount.GetOffsetChecked(), Member_ItemEntryGuid.GetOffsetChecked()))
    {
        CallVirtual<void>(this, Natives::AFortWeapon_ModAmmoIndex, AmmoDelta);
    }
}

void AFortWeapon::DestroyedDueToDurability()
{
    if (Natives::AFortWeapon_DestroyedDueToDurabilityIndex >= 0)
    {
        CallVirtual<void>(this, Natives::AFortWeapon_DestroyedDueToDurabilityIndex);
    }
}

// 1.7.2 renamed bPendingDestroyDueToDurability to bPendingDestroyDueToDurabilityOrStackCount (same place and bit).
Reflection::FMemberInfo& AFortWeapon::PendingDestroyDueToDurabilityMember()
{
    return Member_bPendingDestroyDueToDurability.IsAvailable() ? Member_bPendingDestroyDueToDurability : Member_bPendingDestroyDueToDurabilityOrStackCount;
}

bool AFortWeapon::IsPendingDestroyDueToDurability() const
{
    return Reflection::GetBoolMember(this, PendingDestroyDueToDurabilityMember());
}

ptrdiff_t AFortWeapon::PendingDestroyDueToDurabilityOffset()
{
    return PendingDestroyDueToDurabilityMember().GetOffsetChecked();
}

uint8_t AFortWeapon::PendingDestroyDueToDurabilityMask()
{
    Reflection::FMemberInfo& Member = PendingDestroyDueToDurabilityMember();
    Member.GetOffsetChecked();
    return Member.GetFieldMask();
}
