#pragma once

#include "AttributeSet.h"
#include "GameplayTagContainer.h"
#include "UObject/Object.h"

class AActor;
class UAbilitySystemComponent;

class UAbilitySystemBlueprintLibrary : public UObject
{
    DefineUnrealClass(UAbilitySystemBlueprintLibrary, GameplayAbilities)
    DefineUFunction(GetAbilitySystemComponent)
    DefineUFunction(EvaluateAttributeValueWithTags)
    DefineUFunction(EvaluateAttributeValueWithTagsAndBase)

public:
    static UAbilitySystemComponent* GetAbilitySystemComponent(AActor* Actor);
    static float EvaluateAttributeValueWithTags(UAbilitySystemComponent* AbilitySystem, const FGameplayAttribute& Attribute, const FGameplayTagContainer& SourceTags, const FGameplayTagContainer& TargetTags, bool& bSuccess);
    static float EvaluateAttributeValueWithTagsAndBase(UAbilitySystemComponent* AbilitySystem, const FGameplayAttribute& Attribute, const FGameplayTagContainer& SourceTags, const FGameplayTagContainer& TargetTags, float BaseValue, bool& bSuccess);
};
