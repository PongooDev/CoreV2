#pragma once

#include "AttributeSet.h"
#include "GameplayAbilitySpec.h"
#include "GameplayPrediction.h"
#include "GameplayTasksComponent.h"

class AActor;
class UGameplayAbility;
struct FGameplayEventData;
struct FGameplayTagContainer;

class UAbilitySystemComponent : public UGameplayTasksComponent
{
    DefineUnrealClass(UAbilitySystemComponent, GameplayAbilities)

public:
    DefineUStructProperty(FGameplayAbilitySpecContainer, ActivatableAbilities)
    DefineUProperty(AActor*, OwnerActor)
    DefineUProperty(TArray<UAttributeSet*>, SpawnedAttributes)
    DefineUFunction(ClientActivateAbilityFailed)
    DefineUFunction(ServerTryActivateAbility)
    DefineUFunction(RemoveActiveGameplayEffectBySourceEffect)

    template <class T>
    const T* GetSet() const
    {
        for (UAttributeSet* Set : SpawnedAttributes)
        {
            if (Set && Set->IsA(T::StaticClass()))
            {
                return static_cast<const T*>(Set);
            }
        }
        return nullptr;
    }

    bool HasAllMatchingGameplayTags(const FGameplayTagContainer& TagContainer) const;
    void GetOwnedGameplayTags(FGameplayTagContainer& TagContainer) const;
    FGameplayAbilitySpec* FindAbilitySpecFromHandle(FGameplayAbilitySpecHandle Handle);
    void MarkAbilitySpecDirty(FGameplayAbilitySpec& Spec);
    // Gives the ability and activates it once; the spec is removed when that activation ends. Server only.
    FGameplayAbilitySpecHandle GiveAbilityAndActivateOnce(const FGameplayAbilitySpec& Spec);
    bool InternalTryActivateAbility(FGameplayAbilitySpecHandle AbilityToActivate, const FPredictionKey& InPredictionKey, UGameplayAbility** OutInstancedAbility = nullptr, void* OnGameplayAbilityEndedDelegate = nullptr, const FGameplayEventData* TriggerEventData = nullptr);
    // Removes StacksToRemove stacks (-1: all) of every active effect of the class, from any instigator. Server only; false
    // when the build reflects no such function.
    bool RemoveActiveGameplayEffectBySourceEffect(UClass* GameplayEffect, UAbilitySystemComponent* InstigatorAbilitySystemComponent = nullptr, int32_t StacksToRemove = -1);
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
