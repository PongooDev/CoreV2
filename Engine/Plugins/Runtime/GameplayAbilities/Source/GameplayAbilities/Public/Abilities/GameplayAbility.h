#pragma once

#include "GameplayTagContainer.h"
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
    DefineUProperty(FGameplayTagContainer, AbilityTags)
    DefineUProperty(FGameplayTagContainer, ActivationBlockedTags)
    DefineUProperty(FGameplayTagContainer, ActivationRequiredTags)
    DefineUProperty(uint8_t, NetExecutionPolicy)
    DefineUProperty(uint8_t, InstancingPolicy)

    // DoesAbilitySatisfyTagRequirements' owner checks, in words: the owned tags among ActivationBlockedTags and the
    // ActivationRequiredTags the owner lacks (empty when both pass or the build doesn't reflect them).
    std::wstring DescribeTagRequirements(const FGameplayTagContainer& OwnedTags) const;
    // The InstancingPolicy and NetExecutionPolicy values, or -1 when the build doesn't reflect them.
    int32_t GetInstancingPolicyValue() const;
    int32_t GetNetExecutionPolicyValue() const;

    void K2_EndAbility();
    // CancelAbility on the current activation, replicated: K2_OnEndAbility runs with bWasCancelled, on the server and on
    // the owner's client.
    void K2_CancelAbility();
};
