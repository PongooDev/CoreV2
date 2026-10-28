#pragma once

#include "Abilities/FortPlayerAttrSet.h"
#include "Containers/Array.h"
#include "GameFramework/Pawn.h"
#include "Misc/Guid.h"
#include "UObject/ScriptDelegates.h"

class AFortWeapon;
class UAbilitySystemComponent;
class UFortWeaponItemDefinition;

class AFortPawn : public APawn
{
    DefineUnrealClass(AFortPawn, FortniteGame)
    DefineUFunction(IsDead)
    DefineUFunction(EquipWeaponDefinition)
    DefineUFunction(TriggerFeedbackEvent)
    DefineUFunction(GetHealth)
    DefineUFunction(GetMaxHealth)
    DefineUFunction(GetShield)
    DefineUFunction(GetMaxShield)

public:
    DefineUProperty(AFortWeapon*, CurrentWeapon)
    DefineUProperty(TArray<AFortWeapon*>, CurrentWeaponList)
    DefineUProperty(FMulticastScriptDelegate, OnDied)
    DefineUProperty(FName, SpottedEvent)
    // Down But Not Out: knocked down until a teammate revives the pawn or it bleeds out.
    DefineUProperty(bool, bIsDBNO)
    DefineUProperty(bool, bPlayedDying)
    DefineUProperty(UFortHealthSet*, HealthSet)
    // What the pawn's IAbilitySystemInterface returns (its player state's, for a player).
    DefineUProperty(UAbilitySystemComponent*, AbilitySystemComponent)

    bool IsDead() const;
    AFortWeapon* EquipWeaponDefinition(const UFortWeaponItemDefinition* WeaponData, const FGuid& ItemEntryGuid);
    bool EquipDefaultWeapon();
    void RemoveAbilityWeapon(AFortWeapon* Weapon);
    void TriggerFeedbackEvent(FName EventName, AFortPawn* InstigatorPawn, AFortPawn* Recipient, float OverriddenDelay);
    float GetHealth() const;
    float GetMaxHealth() const;
    float GetShield() const;
    float GetMaxShield() const;

    static ptrdiff_t CurrentWeaponListOffset();
    static ptrdiff_t IsDBNOOffset();
};
