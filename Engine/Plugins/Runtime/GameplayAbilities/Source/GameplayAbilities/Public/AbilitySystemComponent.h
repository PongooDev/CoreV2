#pragma once

#include "AttributeSet.h"
#include "Delegates/Delegate.h"
#include "GameplayAbilitySpec.h"
#include "GameplayEffect.h"
#include "GameplayPrediction.h"
#include "GameplayTasksComponent.h"
#include "Natives.h"

class AActor;
class UGameplayAbility;
struct FGameplayEventData;
struct FGameplayTagContainer;

class UAbilitySystemComponent : public UGameplayTasksComponent
{
    DefineUnrealClass(UAbilitySystemComponent, GameplayAbilities)

public:
    // DECLARE_MULTICAST_DELEGATE_ThreeParams(FOnGameplayEffectAppliedDelegate, UAbilitySystemComponent*,
    // const FGameplayEffectSpec&, FActiveGameplayEffectHandle).
    using FOnGameplayEffectAppliedDelegate = TMulticastDelegate<UAbilitySystemComponent*, const FGameplayEffectSpec&, FActiveGameplayEffectHandle>;

    // Called on both client and server whenever a duration based effect is added (Epic's comment): an effect's
    // modifiers and granted tags go on (FActiveGameplayEffectsContainer::AddActiveGameplayEffectGrantedTagsAndModifiers),
    // an ability's cooldown among them. Unreflected: only where ResolveNatives found it (HasOnActiveGameplayEffectAdded).
    DefineNativeObject(FOnGameplayEffectAppliedDelegate, OnActiveGameplayEffectAddedDelegateToSelf, UAbilitySystemComponent_OnActiveGameplayEffectAddedDelegateToSelf)
    DefineUStructProperty(FActiveGameplayEffectsContainer, ActiveGameplayEffects)
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
    DefineUFunction(GetGameplayEffectCount)
    DefineUFunction(SetActiveGameplayEffectLevelUsingQuery)
    DefineUFunction(TryActivateAbilitiesByTag)

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
    // Tries to activate every ability whose AbilityTags have all of the container's tags (the engine's, BlueprintCallable):
    // true when any did. A locally controlled owner (a server-side bot) activates them here, a client predicts them.
    bool TryActivateAbilitiesByTag(const FGameplayTagContainer& GameplayTagContainer, bool bAllowRemoteActivation = true);
    // Cancels every active ability whose AbilityTags have all of WithTags and none of WithoutTags, Ignore aside. The build's
    // own where Core found it (Finder::ResolveDBNOReviveLive); otherwise the engine's CancelAbilitySpec per spec: each
    // instance's CancelAbility on its current activation (K2_CancelAbility), replicated to the owner's client.
    void CancelAbilities(const FGameplayTagContainer* WithTags = nullptr, const FGameplayTagContainer* WithoutTags = nullptr, UGameplayAbility* Ignore = nullptr);
    // Removes StacksToRemove stacks (-1: all) of every active effect of the class, from any instigator. Server only; false
    // when the build reflects no such function.
    bool RemoveActiveGameplayEffectBySourceEffect(UClass* GameplayEffect, UAbilitySystemComponent* InstigatorAbilitySystemComponent = nullptr, int32_t StacksToRemove = -1);
    // How many active effects of the class the ability system has (the engine's, BlueprintCallable): with
    // bEnforceOnGoingCheck, only those whose ongoing tag requirements hold. -1 where the build reflects no such function.
    int32_t GetGameplayEffectCount(UClass* SourceGameplayEffect, UAbilitySystemComponent* OptionalInstigatorFilterComponent = nullptr, bool bEnforceOnGoingCheck = true);
    // Sets the level of every active effect of the class (the engine's SetActiveGameplayEffectLevelUsingQuery with a query
    // on the effect's definition). Server only; false where the build reflects no such function.
    bool SetActiveGameplayEffectLevelUsingQuery(UClass* EffectDefinition, int32_t NewLevel);
    // The ability's input was let go (a server RPC: on the server it runs here, AbilitySpecInputReleased): an active
    // ability waiting for its release (an automatic weapon's fire) ends.
    void ServerSetInputReleased(FGameplayAbilitySpecHandle AbilityHandle);
    void ClientActivateAbilityFailed(FGameplayAbilitySpecHandle AbilityToActivate, int16_t PredictionKey);
    void InternalServerTryActiveAbility(FGameplayAbilitySpecHandle AbilityToActivate, bool InputPressed, const FPredictionKey& PredictionKey, const FGameplayEventData* TriggerEventData);

    void IncrementAbilityListLock();
    void DecrementAbilityListLock();

    // Epic's inline accessor: ActiveGameplayEffects.RegisterGameplayAttributeEvent(Attribute), the delegate the
    // attribute's every change of current value broadcasts (an effect added, removed or changed, its base value set).
    // Null where the build's FindOrAdd wasn't found.
    FOnGameplayAttributeChange* RegisterGameplayAttributeEvent(FGameplayAttribute Attribute);

    // Finds what the delegates above need in the build, once (game thread; logged): OnActiveGameplayEffectAddedDelegate
    // ToSelf's offset from AddActiveGameplayEffectGrantedTagsAndModifiers' broadcast (its log line "...called with null
    // Def!"), and FActiveGameplayEffectsContainer::RegisterGameplayAttributeEvent from InternalUpdateNumericalAttribute's
    // ("Property %s new value is: %.2f") lookup of AttributeChangeDelegates. False where either isn't found.
    static bool ResolveNatives();
    // Each of the two, and the build's delegate instance layout Core binds with (TMulticastDelegate::Add refuses without).
    static bool HasOnActiveGameplayEffectAdded() { ResolveNatives(); return Natives::UAbilitySystemComponent_OnActiveGameplayEffectAddedDelegateToSelf > 0 && Natives::IDelegateInstance_GetHandleIndexResolved; }
    static bool HasGameplayAttributeEvents() { ResolveNatives(); return Natives::FActiveGameplayEffectsContainer_RegisterGameplayAttributeEvent && Natives::IDelegateInstance_GetHandleIndexResolved; }

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
