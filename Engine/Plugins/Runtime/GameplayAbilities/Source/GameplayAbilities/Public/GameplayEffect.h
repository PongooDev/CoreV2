#pragma once

#include "AttributeSet.h"
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

struct FCurveTableRowHandle
{
    DefineUnrealStruct(FCurveTableRowHandle, Engine)
    DefineUProperty(UObject*, CurveTable)
    DefineUProperty(FName, RowName)
};

// A float, scaled by the curve's row at the effect's level when it has one.
struct FScalableFloat
{
    DefineUnrealStruct(FScalableFloat, GameplayAbilities)
    DefineUProperty(float, Value)
    DefineUStructProperty(FCurveTableRowHandle, Curve)
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
