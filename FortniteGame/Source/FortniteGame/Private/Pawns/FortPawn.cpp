#include "pch.h"
#include "Pawns/FortPawn.h"
#include "Finder.h"
#include "Natives.h"
#include "ObjectLayout.h"
#include "Pawns/FortPlayerPawn.h"
#include "ReflectionParams.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "Weapons/FortWeapon.h"

bool AFortPawn::IsDead() const
{
    Reflection::FParams Params(StaticFunction_IsDead());
    return Params.Call(const_cast<AFortPawn*>(this)) && Params.Take<bool>(L"ReturnValue");
}

AFortWeapon* AFortPawn::EquipWeaponDefinition(const UFortWeaponItemDefinition* WeaponData, const FGuid& ItemEntryGuid)
{
    Reflection::FParams Params(StaticFunction_EquipWeaponDefinition());
    Params.Set(L"WeaponData", const_cast<UFortWeaponItemDefinition*>(WeaponData));
    Params.Set(L"ItemEntryGuid", ItemEntryGuid);
    return Params.Call(this) ? Params.Take<AFortWeapon*>(L"ReturnValue") : nullptr;
}

bool AFortPawn::EquipDefaultWeapon()
{
    if (!Finder::ResolveAbilityWeaponLive(CurrentWeaponListOffset(), AFortPlayerPawn::LastEquippedWeaponGUIDOffset()))
    {
        return false;
    }
    return CallVirtual<bool>(this, Natives::AFortPawn_EquipDefaultWeaponIndex);
}

void AFortPawn::RemoveAbilityWeapon(AFortWeapon* Weapon)
{
#if WITH_SERVER_CODE
    if (Role != ROLE_Authority || !Weapon)
    {
        return;
    }

    TArray<AFortWeapon*>& Weapons = CurrentWeaponList;
    for (int32_t Index = Weapons.Num() - 1; Index >= 0; --Index)
    {
        if (Weapons[Index] == Weapon)
        {
            Weapons.RemoveAt(Index);
        }
    }

    if (static_cast<AFortWeapon*>(CurrentWeapon) == Weapon)
    {
        AFortPlayerPawn* PlayerPawn = Cast<AFortPlayerPawn>(this);
        if (!PlayerPawn || !PlayerPawn->EquipLastWeapon())
        {
            EquipDefaultWeapon();
        }
    }
    Weapon->K2_DestroyActor();
#endif
}

ptrdiff_t AFortPawn::CurrentWeaponListOffset()
{
    return Member_CurrentWeaponList.GetOffsetChecked();
}
