#pragma once

#include "GameplayAbilitySpec.h"
#include "GameplayPrediction.h"
#include "GameplayTasksComponent.h"

class AActor;
class UGameplayAbility;
struct FGameplayEventData;

class UAbilitySystemComponent : public UGameplayTasksComponent
{
    DefineUnrealClass(UAbilitySystemComponent, GameplayAbilities)

public:
    DefineUStructProperty(FGameplayAbilitySpecContainer, ActivatableAbilities)
    DefineUProperty(AActor*, OwnerActor)
    DefineUFunction(ClientActivateAbilityFailed)
    DefineUFunction(ServerTryActivateAbility)

    FGameplayAbilitySpec* FindAbilitySpecFromHandle(FGameplayAbilitySpecHandle Handle);
    void MarkAbilitySpecDirty(FGameplayAbilitySpec& Spec);
    bool InternalTryActivateAbility(FGameplayAbilitySpecHandle AbilityToActivate, const FPredictionKey& InPredictionKey, UGameplayAbility** OutInstancedAbility = nullptr, void* OnGameplayAbilityEndedDelegate = nullptr, const FGameplayEventData* TriggerEventData = nullptr);
    void ClientActivateAbilityFailed(FGameplayAbilitySpecHandle AbilityToActivate, int16_t PredictionKey);
    void InternalServerTryActiveAbility(FGameplayAbilitySpecHandle AbilityToActivate, bool InputPressed, const FPredictionKey& PredictionKey, const FGameplayEventData* TriggerEventData);

    void IncrementAbilityListLock();
    void DecrementAbilityListLock();

    static void HookVTables();

private:
    static void InternalServerTryActiveAbilityHK(UAbilitySystemComponent* This, FGameplayAbilitySpecHandle AbilityToActivate, bool InputPressed, const FPredictionKey& PredictionKey, const FGameplayEventData* TriggerEventData);
};

class FScopedAbilityListLock
{
public:
    FScopedAbilityListLock(UAbilitySystemComponent& InAbilitySystemComponent)
        : AbilitySystemComponent(InAbilitySystemComponent)
    {
        AbilitySystemComponent.IncrementAbilityListLock();
    }

    ~FScopedAbilityListLock()
    {
        AbilitySystemComponent.DecrementAbilityListLock();
    }

private:
    UAbilitySystemComponent& AbilitySystemComponent;
};

#define ABILITYLIST_SCOPE_LOCK() FScopedAbilityListLock ActiveScopeLock(*this);
