#include "pch.h"
#include "Abilities/FortGameplayAbility.h"
#include "Finder.h"
#include "Hooks.h"
#include "Items/FortInventory.h"
#include "Items/FortItem.h"
#include "Items/FortItemDefinition.h"
#include "Log.h"
#include "Natives.h"
#include "Pawns/FortPawn.h"
#include "Player/FortPlayerController.h"
#include "ReflectionParams.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "Weapons/FortWeapon.h"

AFortPawn* UFortGameplayAbility::GetActivatingPawn() const
{
    Reflection::FParams Params(StaticFunction_GetActivatingPawn());
    return Params.Call(const_cast<UFortGameplayAbility*>(this)) ? Params.Take<AFortPawn*>(L"ReturnValue") : nullptr;
}

TWeakObjectPtr<UFortWorldItem>& UFortGameplayAbility::GetAbilityGrantedWeaponItem() const
{
    const ptrdiff_t Offset = Member_SmallPreviewImageOverride.GetOffsetChecked() + sizeof(UTexture2D*);
    return *reinterpret_cast<TWeakObjectPtr<UFortWorldItem>*>(reinterpret_cast<uintptr_t>(this) + Offset);
}

void UFortGameplayAbility::EquipAbilityWeapon(UFortWeaponItemDefinition* WeaponDefinition, int32_t ItemLevel)
{
#if WITH_SERVER_CODE
    AFortPawn* Pawn = GetActivatingPawn();
    if (!Pawn || Pawn->Role != ROLE_Authority || !WeaponDefinition)
    {
        return;
    }

    AFortPlayerController* PlayerController = Cast<AFortPlayerController>(reinterpret_cast<UObject*>(static_cast<AController*>(Pawn->Controller)));
    AFortInventory* Inventory = PlayerController ? static_cast<AFortInventory*>(PlayerController->WorldInventory) : nullptr;
    if (!Inventory)
    {
        return;
    }

    UFortWorldItem* WorldItem = Cast<UFortWorldItem>(WeaponDefinition->CreateTemporaryItemInstanceBP(1, ItemLevel));
    if (!WorldItem)
    {
        return;
    }

    WorldItem->SetOwningControllerForTemporaryItem(PlayerController);
    Inventory->AddWorldItem(WorldItem);
    GetAbilityGrantedWeaponItem() = TWeakObjectPtr<UFortWorldItem>(WorldItem);

    FFortItemEntry& ItemEntry = WorldItem->ItemEntry;
    const FGuid ItemGuid = ItemEntry.ItemGuid;
    Pawn->EquipWeaponDefinition(WeaponDefinition, ItemGuid);
#endif
}

void UFortGameplayAbility::RemoveAbilityWeapon()
{
#if WITH_SERVER_CODE
    UFortWorldItem* WorldItem = GetAbilityGrantedWeaponItem().Get();
    AFortPawn* Pawn = WorldItem ? GetActivatingPawn() : nullptr;
    if (!Pawn || Pawn->Role != ROLE_Authority)
    {
        return;
    }

    FFortItemEntry& ItemEntry = WorldItem->ItemEntry;
    const FGuid ItemGuid = ItemEntry.ItemGuid;
    TArray<AFortWeapon*>& Weapons = Pawn->CurrentWeaponList;
    for (int32_t Index = 0; Index < Weapons.Num(); ++Index)
    {
        AFortWeapon* Weapon = Weapons[Index];
        if (Weapon && Weapon->ItemEntryGuid == ItemGuid)
        {
            Pawn->RemoveAbilityWeapon(Weapon);
            return;
        }
    }
#endif
}

DEFINE_FUNCTION(UFortGameplayAbility::execEquipAbilityWeapon)
{
    P_GET_OBJECT(UFortWeaponItemDefinition, Z_Param_WeaponDefinition);
    P_GET_PROPERTY(UIntProperty, Z_Param_ItemLevel);
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<UFortGameplayAbility*>(Context)->EquipAbilityWeapon(Z_Param_WeaponDefinition, Z_Param_ItemLevel);
    P_NATIVE_END;
}

void UFortGameplayAbility::HookNatives()
{
    if (UFunction* Function = StaticFunction_EquipAbilityWeapon(); Function && (Natives::FFrame_Step || Finder::ResolveScriptFrame()))
    {
        Function->SetNativeFunc(&execEquipAbilityWeapon);
        Log::Info(L"Hook UFortGameplayAbility::EquipAbilityWeapon: native");
    }

    const ptrdiff_t AbilityGrantedWeaponItemOffset = Member_SmallPreviewImageOverride.GetOffsetChecked() + sizeof(UTexture2D*);
    if (uint8_t* Call = Finder::AbilityEndCleanupCall(AbilityGrantedWeaponItemOffset))
    {
        EndAbilityCleanupOG = reinterpret_cast<decltype(EndAbilityCleanupOG)>(Finder::RelativeTarget(reinterpret_cast<uintptr_t>(Call), 1, 5));
        Hooks::RedirectCall(L"UFortGameplayAbility::EndAbility -> RemoveAbilityWeapon", Call, reinterpret_cast<void*>(&EndAbilityCleanupCall));
    }
}

void UFortGameplayAbility::EndAbilityCleanupCall(UFortGameplayAbility* This)
{
    This->RemoveAbilityWeapon();
    EndAbilityCleanupOG(This);
}
