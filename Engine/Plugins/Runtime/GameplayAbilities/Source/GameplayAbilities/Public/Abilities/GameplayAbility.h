#pragma once

#include "UObject/Object.h"

class UGameplayAbility : public UObject
{
    DefineUnrealClass(UGameplayAbility, GameplayAbilities)
    DefineUFunction(K2_EndAbility)

public:
    void K2_EndAbility();
};
