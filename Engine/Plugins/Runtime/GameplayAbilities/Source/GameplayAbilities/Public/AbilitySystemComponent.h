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
    // Reflected from UE 4.21 (5.0) on; older builds keep them unreflected (IsAvailable).
    DefineUProperty(AActor*, AvatarActor)
    DefineUProperty(bool, UserAbilityActivationInhibited)
    DefineUProperty(TArray<uint8_t>, BlockedAbilityBindings)
    DefineUProperty(TArray<UAttributeSet*>, SpawnedAttributes)
    DefineUFunction(ClientActivateAbilityFailed)
    DefineUFunction(ServerTryActivateAbility)
    DefineUFunction(ServerSetInputReleased)
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
    // The spec whose ability is AbilityClass's default object, or null.
    FGameplayAbilitySpec* FindAbilitySpecFromClass(UClass* AbilityClass);
    void MarkAbilitySpecDirty(FGameplayAbilitySpec& Spec);
    // Gives the ability and activates it once; the spec is removed when that activation ends. Server only.
    FGameplayAbilitySpecHandle GiveAbilityAndActivateOnce(const FGameplayAbilitySpec& Spec);
    // The engine's GiveAbility: adds the spec to ActivatableAbilities (replicated to the owner) and returns its handle; an
    // invalid handle when it isn't the authority or the build's GiveAbility wasn't found. Server only.
    FGameplayAbilitySpecHandle GiveAbility(const FGameplayAbilitySpec& Spec);
    // TryActivateAbility as the authority runs it: InternalTryActivateAbility with no prediction key. An instanced-per-actor
    // ability that is active and bRetriggerInstancedAbility is ended first, then activated again.
    bool TryActivateAbility(FGameplayAbilitySpecHandle AbilityToActivate);
    bool InternalTryActivateAbility(FGameplayAbilitySpecHandle AbilityToActivate, const FPredictionKey& InPredictionKey, UGameplayAbility** OutInstancedAbility = nullptr, void* OnGameplayAbilityEndedDelegate = nullptr, const FGameplayEventData* TriggerEventData = nullptr);
    // Removes StacksToRemove stacks (-1: all) of every active effect of the class, from any instigator. Server only; false
    // when the build reflects no such function.
    bool RemoveActiveGameplayEffectBySourceEffect(UClass* GameplayEffect, UAbilitySystemComponent* InstigatorAbilitySystemComponent = nullptr, int32_t StacksToRemove = -1);
    // The ability's input was let go (a server RPC: on the server it runs here, AbilitySpecInputReleased): an active
    // ability waiting for its release (an automatic weapon's fire) ends.
    void ServerSetInputReleased(FGameplayAbilitySpecHandle AbilityHandle);
    void ClientActivateAbilityFailed(FGameplayAbilitySpecHandle AbilityToActivate, int16_t PredictionKey);
    void InternalServerTryActiveAbility(FGameplayAbilitySpecHandle AbilityToActivate, bool InputPressed, const FPredictionKey& PredictionKey, const FGameplayEventData* TriggerEventData);

    void IncrementAbilityListLock();
    void DecrementAbilityListLock();

    static void HookVTables();

private:
    // Why the native InternalTryActivateAbility refused a client's activation, from the state its checks read (actor
    // info, the avatar's role, UserAbilityActivationInhibited, input blocking, the ability's tag requirements).
    FString DescribeActivationFailure(const FGameplayAbilitySpec& Spec) const;

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
