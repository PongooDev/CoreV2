#pragma once

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
