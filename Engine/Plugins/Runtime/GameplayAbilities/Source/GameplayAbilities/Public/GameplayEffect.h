#pragma once

#include "AttributeSet.h"
#include "Delegates/Delegate.h"
#include "Engine/CurveTable.h"
#include "GameplayTagContainer.h"
#include "StructArray.h"
#include "UObject/Object.h"

class EGameplayModOp
{
    DefineUnrealEnum(EGameplayModOp, GameplayAbilities)
    DefineUEnumValue(Additive)
    DefineUEnumValue(Multiplicitive)
};

class EGameplayEffectMagnitudeCalculation
{
    DefineUnrealEnum(EGameplayEffectMagnitudeCalculation, GameplayAbilities)
    DefineUEnumValue(ScalableFloat)
};

// A float, scaled by the curve's row at the effect's level when it has one.
struct FScalableFloat
{
    DefineUnrealStruct(FScalableFloat, GameplayAbilities)
    DefineUProperty(float, Value)
    DefineUStructProperty(FCurveTableRowHandle, Curve)

    // Value, scaled by its curve table row at Level when it has one (Value alone when the row is missing).
    float GetValueAtLevel(float Level) const;
};

struct FGameplayEffectModifierMagnitude
{
    DefineUnrealStruct(FGameplayEffectModifierMagnitude, GameplayAbilities)
    DefineUProperty(EGameplayEffectMagnitudeCalculation, MagnitudeCalculationType)
    DefineUStructProperty(FScalableFloat, ScalableFloatMagnitude)
};

// The tags a modifier needs on the source (or target) for it to count: a weapon perk's "weapon.ranged" or "weapon.melee".
struct FGameplayTagRequirements
{
    DefineUnrealStruct(FGameplayTagRequirements, GameplayAbilities)
    DefineUProperty(FGameplayTagContainer, RequireTags)
};

struct FGameplayModifierInfo
{
    DefineUnrealStruct(FGameplayModifierInfo, GameplayAbilities)
    DefineUProperty(FGameplayAttribute, Attribute)
    DefineUProperty(EGameplayModOp, ModifierOp)
    DefineUStructProperty(FGameplayEffectModifierMagnitude, ModifierMagnitude)
    DefineUStructProperty(FGameplayTagRequirements, SourceTags)

    // The fixed amount this modifier adds to Attribute: an additive, curveless scalable float, else 0.
    float GetAdditiveConstant(const UProperty* InAttribute) const;
};

class UGameplayEffect : public UObject
{
    DefineUnrealClass(UGameplayEffect, GameplayAbilities)

public:
    DefineUStructArrayProperty(FGameplayModifierInfo, Modifiers)
};

struct FGameplayEffectModCallbackData;

// Epic's FActiveGameplayEffectHandle: an active effect's handle. 8 bytes, so it goes by value in a register.
struct FActiveGameplayEffectHandle
{
    int32_t Handle = -1;
    bool bPassedFiltersAndWasExecuted = false;

    bool IsValid() const { return Handle != -1; }
};

static_assert(sizeof(FActiveGameplayEffectHandle) == 8);

// Epic's FGameplayEffectSpec: an effect to apply or applied, its definition (Def, the effect class's default object) with
// the level, context and magnitudes it was made with.
struct FGameplayEffectSpec
{
    DefineUnrealStruct(FGameplayEffectSpec, GameplayAbilities)
    DefineUProperty(UGameplayEffect*, Def)
};

// DECLARE_MULTICAST_DELEGATE_TwoParams(FOnGameplayAttributeChange, float, const FGameplayEffectModCallbackData*): an
// attribute's current value was worked out again (FActiveGameplayEffectsContainer::InternalUpdateNumericalAttribute, after
// SetNumericAttribute_Internal), with the new value. The one UE 4.16 has; 4.17 adds FOnGameplayAttributeValueChange
// beside it, and every build up to 5.40 broadcasts both.
using FOnGameplayAttributeChange = TMulticastDelegate<float, const FGameplayEffectModCallbackData*>;

// Epic's FActiveGameplayEffectsContainer (UAbilitySystemComponent::ActiveGameplayEffects): the active effects, the
// attributes' aggregators and the delegates their changes broadcast.
struct FActiveGameplayEffectsContainer
{
    DefineUnrealStruct(FActiveGameplayEffectsContainer, GameplayAbilities)

public:
    // AttributeChangeDelegates.FindOrAdd(Attribute), through the build's own (UAbilitySystemComponent::ResolveNatives):
    // the delegate InternalUpdateNumericalAttribute broadcasts each time the attribute's current value is worked out
    // again. Null where it wasn't found.
    FOnGameplayAttributeChange* RegisterGameplayAttributeEvent(FGameplayAttribute Attribute);
};
