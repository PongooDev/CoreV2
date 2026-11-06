#pragma once

#include "UObject/Object.h"

class UGameplayAbility : public UObject
{
    DefineUnrealClass(UGameplayAbility, GameplayAbilities)
    DefineUFunction(K2_EndAbility)
    DefineUFunction(K2_CancelAbility)

public:
    // The effect CommitAbility applies to its owner as its cooldown (its granted tags are what CheckCooldown tests): a
    // hero ability's, a gadget's (GA_AirStrike: GE_AirStrike_Cooldown), or null for one without a cooldown.
    DefineUProperty(UClass*, CooldownGameplayEffectClass)

    void K2_EndAbility();
    // CancelAbility on the current activation, replicated: K2_OnEndAbility runs with bWasCancelled, on the server and on
    // the owner's client.
    void K2_CancelAbility();
};
