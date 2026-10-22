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
