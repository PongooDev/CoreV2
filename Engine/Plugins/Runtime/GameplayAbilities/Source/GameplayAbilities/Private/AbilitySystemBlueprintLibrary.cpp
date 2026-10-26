#include "pch.h"
#include "AbilitySystemBlueprintLibrary.h"
#include "AbilitySystemComponent.h"
#include "ReflectionParams.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

FGameplayAttribute::FGameplayAttribute(UProperty* NewProperty)
    : Attribute(NewProperty)
{
    if (Attribute)
    {
        for (UObject* Outer = Attribute->GetOuter(); Outer; Outer = Outer->GetOuter())
        {
            if (Outer->IsA(UStruct::StaticClass()))
            {
                AttributeOwner = static_cast<UStruct*>(Outer);
                break;
            }
        }
        AttributeName = Attribute->GetName();
    }
}

UAbilitySystemComponent* UAbilitySystemBlueprintLibrary::GetAbilitySystemComponent(AActor* Actor)
{
    Reflection::FParams Params(StaticFunction_GetAbilitySystemComponent());
    Params.Set(L"Actor", Actor);
    return Params.Call(StaticClass()->GetDefaultObject()) ? Params.Take<UAbilitySystemComponent*>(L"ReturnValue") : nullptr;
}

float UAbilitySystemBlueprintLibrary::EvaluateAttributeValueWithTags(UAbilitySystemComponent* AbilitySystem, const FGameplayAttribute& Attribute, const FGameplayTagContainer& SourceTags, const FGameplayTagContainer& TargetTags, bool& bSuccess)
{
    Reflection::FParams Params(StaticFunction_EvaluateAttributeValueWithTags(), true);
    Params.Set(L"AbilitySystem", AbilitySystem);
    Params.Set(L"Attribute", Attribute);
    Params.Set(L"SourceTags", SourceTags);
    Params.Set(L"TargetTags", TargetTags);
    bSuccess = false;
    if (!Params.Call(StaticClass()->GetDefaultObject()))
    {
        return 0.0f;
    }
    bSuccess = Params.Take<bool>(L"bSuccess");
    return Params.Take<float>(L"ReturnValue");
}

float UAbilitySystemBlueprintLibrary::EvaluateAttributeValueWithTagsAndBase(UAbilitySystemComponent* AbilitySystem, const FGameplayAttribute& Attribute, const FGameplayTagContainer& SourceTags, const FGameplayTagContainer& TargetTags, float BaseValue, bool& bSuccess)
{
    Reflection::FParams Params(StaticFunction_EvaluateAttributeValueWithTagsAndBase(), true);
    Params.Set(L"AbilitySystem", AbilitySystem);
    Params.Set(L"Attribute", Attribute);
    Params.Set(L"SourceTags", SourceTags);
    Params.Set(L"TargetTags", TargetTags);
    Params.Set(L"BaseValue", BaseValue);
    bSuccess = false;
    if (!Params.Call(StaticClass()->GetDefaultObject()))
    {
        return 0.0f;
    }
    bSuccess = Params.Take<bool>(L"bSuccess");
    return Params.Take<float>(L"ReturnValue");
}
