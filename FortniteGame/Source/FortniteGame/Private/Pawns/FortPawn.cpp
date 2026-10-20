#include "pch.h"
#include "Pawns/FortPawn.h"
#include "ReflectionParams.h"

AFortWeapon* AFortPawn::EquipWeaponDefinition(const UFortWeaponItemDefinition* WeaponData, const FGuid& ItemEntryGuid)
{
    Reflection::FParams Params(StaticFunction_EquipWeaponDefinition());
    Params.Set(L"WeaponData", WeaponData);
    Params.Set(L"ItemEntryGuid", ItemEntryGuid);
    return Params.Call(this) ? Params.Take<AFortWeapon*>(L"ReturnValue") : nullptr;
}
