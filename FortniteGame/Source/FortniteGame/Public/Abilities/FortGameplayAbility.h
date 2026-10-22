#pragma once

#include "Abilities/GameplayAbility.h"
#include "UObject/Stack.h"
#include "UObject/WeakObjectPtr.h"

class AFortPawn;
class UFortWeaponItemDefinition;
class UFortWorldItem;
class UTexture2D;

class UFortGameplayAbility : public UGameplayAbility
{
    DefineUnrealClass(UFortGameplayAbility, FortniteGame)
    DefineUFunction(EquipAbilityWeapon)
    DefineUFunction(GetActivatingPawn)

public:
    DefineUProperty(UTexture2D*, SmallPreviewImageOverride)

    AFortPawn* GetActivatingPawn() const;
    void EquipAbilityWeapon(UFortWeaponItemDefinition* WeaponDefinition, int32_t ItemLevel);
    void RemoveAbilityWeapon();

    TWeakObjectPtr<UFortWorldItem>& GetAbilityGrantedWeaponItem() const;

    static void HookNatives();

    DECLARE_FUNCTION(execEquipAbilityWeapon);

private:
    static inline void (*EndAbilityCleanupOG)(UFortGameplayAbility* This) = nullptr;
    static void EndAbilityCleanupCall(UFortGameplayAbility* This);
};
