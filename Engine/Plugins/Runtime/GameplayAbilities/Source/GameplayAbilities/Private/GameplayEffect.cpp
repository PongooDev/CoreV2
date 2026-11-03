#include "pch.h"
#include "GameplayEffect.h"
#include "ReflectionParams.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"

float FScalableFloat::GetValueAtLevel(float Level) const
{
    const float BaseValue = Value;
    const FCurveTableRowHandle& RowHandle = Curve;
    UCurveTable* CurveTable = RowHandle.CurveTable;
    if (!CurveTable)
    {
        return BaseValue;
    }
    UFunction* Evaluate = FindObject<UFunction>(nullptr, L"/Script/Engine.DataTableFunctionLibrary:EvaluateCurveTableRow");
    UClass* Library = FindObject<UClass>(nullptr, L"/Script/Engine.DataTableFunctionLibrary");
    UObject* LibraryDefault = Library ? Library->GetDefaultObject() : nullptr;
    if (!Evaluate || !LibraryDefault)
    {
        return BaseValue;
    }
    Reflection::FParams Params(Evaluate, true);
    // EEvaluateCurveTableResult::RowFound is 0.
    if (!Params.Set(L"CurveTable", reinterpret_cast<UObject*>(CurveTable)) || !Params.Set(L"RowName", static_cast<FName>(RowHandle.RowName)) || !Params.Set(L"InXY", Level)
        || !Params.Call(LibraryDefault) || Params.Take<uint8_t>(L"OutResult") != 0)
    {
        return BaseValue;
    }
    return BaseValue * Params.Take<float>(L"OutXY");
}

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
