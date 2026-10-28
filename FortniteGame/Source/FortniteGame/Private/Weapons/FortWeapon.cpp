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

void AFortWeap_EditingTool::SetEditActor(ABuildingSMActor* NewEditActor)
{
    // The edit tool's SetEditActor (1.8.1 0x943040, 1.8 0x939510): on the server, the piece it edits, then
    // OnRep_EditActor, the native its exec jumps to.
    if (Role == ROLE_Authority)
    {
        EditActor = NewEditActor;
        Reflection::FParams Params(StaticFunction_OnRep_EditActor());
        Params.Call(this);
    }
}
