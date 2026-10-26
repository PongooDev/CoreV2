#pragma once

#include "Containers/UnrealString.h"
#include "Reflection.h"
#include "UObject/Object.h"

struct FGameplayAttributeData
{
    DefineUnrealStruct(FGameplayAttributeData, GameplayAbilities)
    DefineUProperty(float, BaseValue)
    DefineUProperty(float, CurrentValue)

    float GetBaseValue() const { return BaseValue; }
    float GetCurrentValue() const { return CurrentValue; }
};

class UAttributeSet : public UObject
{
    DefineUnrealClass(UAttributeSet, GameplayAbilities)
};

class UProperty;
class UStruct;

struct FGameplayAttribute
{
    FGameplayAttribute() = default;
    explicit FGameplayAttribute(UProperty* NewProperty);

    bool IsValid() const { return Attribute != nullptr; }

    FString AttributeName;
    UProperty* Attribute = nullptr;
    UStruct* AttributeOwner = nullptr;
};

static_assert(sizeof(FGameplayAttribute) == 0x20);
