#include "pch.h"
#include "GameplayEffect.h"

float FGameplayModifierInfo::GetAdditiveConstant(const UProperty* InAttribute) const
{
    const FGameplayAttribute& ModifiedAttribute = Attribute;
    const FGameplayEffectModifierMagnitude& Magnitude = ModifierMagnitude;
    const FScalableFloat& ScalableFloat = Magnitude.ScalableFloatMagnitude;
    const FCurveTableRowHandle& Curve = ScalableFloat.Curve;
    if (!InAttribute || ModifiedAttribute.Attribute != InAttribute || ModifierOp != EGameplayModOp::GetAdditive()
        || Magnitude.MagnitudeCalculationType != EGameplayEffectMagnitudeCalculation::GetScalableFloat() || Curve.CurveTable)
    {
        return 0.0f;
    }
    return ScalableFloat.Value;
}
