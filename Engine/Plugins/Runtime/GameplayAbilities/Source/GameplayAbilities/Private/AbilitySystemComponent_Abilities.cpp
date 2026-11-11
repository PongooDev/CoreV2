#include "pch.h"
#include "AbilitySystemComponent.h"
#include "Abilities/GameplayAbility.h"
#include "Finder.h"
#include "GameFramework/Actor.h"
#include "Hooks.h"
#include "Logging/LogMacros.h"
#include "Natives.h"
#include "ReflectionParams.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"

#include <vector>

DEFINE_LOG_CATEGORY_STATIC(LogAbilitySystem, Log, All);

#define ABILITY_LOG(Verbosity, Format, ...) UE_LOG(LogAbilitySystem, Verbosity, Format, ##__VA_ARGS__)

FScopedPredictionWindow::FScopedPredictionWindow(UAbilitySystemComponent* AbilitySystemComponent, const FPredictionKey& InPredictionKey, bool InSetReplicatedPredictionKey)
{
    if (!Natives::FScopedPredictionWindow_FScopedPredictionWindow || !Natives::FScopedPredictionWindow_Destructor)
    {
        return;
    }

    UScriptStruct* PredictionKeyStruct = FPredictionKey::StaticStruct();
    std::vector<uint8_t> PredictionKeyCopy(PredictionKeyStruct->GetStructureSize(), 0);
    PredictionKeyStruct->CopyScriptStruct(PredictionKeyCopy.data(), &InPredictionKey);
    Natives::FScopedPredictionWindow_FScopedPredictionWindow(Storage, AbilitySystemComponent, PredictionKeyCopy.data(), InSetReplicatedPredictionKey);
    bConstructed = true;
}

FScopedPredictionWindow::~FScopedPredictionWindow()
{
    if (bConstructed)
    {
        Natives::FScopedPredictionWindow_Destructor(Storage);
    }
}

void UGameplayAbility::K2_EndAbility()
{
    Reflection::FParams Params(StaticFunction_K2_EndAbility());
    Params.Call(this);
}

std::wstring UGameplayAbility::DescribeTagRequirements(const FGameplayTagContainer& OwnedTags) const
{
    std::wstring Description;
    if (Member_ActivationBlockedTags.IsAvailable())
    {
        for (const FGameplayTag& Tag : ActivationBlockedTags.GameplayTags)
        {
            if (OwnedTags.HasTag(Tag))
            {
                Description += L" blocked by owned " + Tag.TagName.ToStdString() + L";";
            }
        }
    }
    if (Member_ActivationRequiredTags.IsAvailable())
    {
        for (const FGameplayTag& Tag : ActivationRequiredTags.GameplayTags)
        {
            if (!OwnedTags.HasTag(Tag))
            {
                Description += L" requires missing " + Tag.TagName.ToStdString() + L";";
            }
        }
    }
    return Description;
}

int32_t UGameplayAbility::GetInstancingPolicyValue() const
{
    return Member_InstancingPolicy.IsAvailable() ? static_cast<int32_t>(InstancingPolicy) : -1;
}

int32_t UGameplayAbility::GetNetExecutionPolicyValue() const
{
    return Member_NetExecutionPolicy.IsAvailable() ? static_cast<int32_t>(NetExecutionPolicy) : -1;
}

void UGameplayAbility::K2_CancelAbility()
{
    Reflection::FParams Params(StaticFunction_K2_CancelAbility());
    Params.Call(this);
}

bool UAbilitySystemComponent::HasAllMatchingGameplayTags(const FGameplayTagContainer& TagContainer) const
{
    if (Natives::UAbilitySystemComponent_GameplayTagAssetInterface < 0 || Natives::IGameplayTagAssetInterface_HasAllMatchingGameplayTagsIndex < 0)
    {
        return false;
    }

    const void* const TagAssetInterface = reinterpret_cast<const uint8_t*>(this) + Natives::UAbilitySystemComponent_GameplayTagAssetInterface;
    using FHasAllMatchingGameplayTags = bool (*)(const void* This, const FGameplayTagContainer& TagContainer);
    return reinterpret_cast<FHasAllMatchingGameplayTags>((*reinterpret_cast<void* const* const*>(TagAssetInterface))[Natives::IGameplayTagAssetInterface_HasAllMatchingGameplayTagsIndex])(TagAssetInterface, TagContainer);
}

void UAbilitySystemComponent::GetOwnedGameplayTags(FGameplayTagContainer& TagContainer) const
{
    if (Natives::IGameplayTagAssetInterface_GetOwnedGameplayTagsIndex < 0)
    {
        Finder::ResolveLootLive();
    }
    if (Natives::UAbilitySystemComponent_GameplayTagAssetInterface < 0 || Natives::IGameplayTagAssetInterface_GetOwnedGameplayTagsIndex < 0)
    {
        return;
    }

    const void* const TagAssetInterface = reinterpret_cast<const uint8_t*>(this) + Natives::UAbilitySystemComponent_GameplayTagAssetInterface;
    using FGetOwnedGameplayTags = void (*)(const void* This, FGameplayTagContainer& TagContainer);
    reinterpret_cast<FGetOwnedGameplayTags>((*reinterpret_cast<void* const* const*>(TagAssetInterface))[Natives::IGameplayTagAssetInterface_GetOwnedGameplayTagsIndex])(TagAssetInterface, TagContainer);
}

FGameplayAbilitySpec* UAbilitySystemComponent::FindAbilitySpecFromHandle(FGameplayAbilitySpecHandle Handle)
{
    FGameplayAbilitySpecContainer& Container = ActivatableAbilities;
    TStructArray<FGameplayAbilitySpec>& Items = Container.Items;
    for (int32_t Index = 0; Index < Items.Num(); ++Index)
    {
        FGameplayAbilitySpec& Spec = Items[Index];
        if (Spec.Handle == Handle)
        {
            return &Spec;
        }
    }

    return nullptr;
}

FGameplayAbilitySpec* UAbilitySystemComponent::FindAbilitySpecFromClass(UClass* AbilityClass)
{
    if (!AbilityClass)
    {
        return nullptr;
    }

    UObject* const AbilityDefault = AbilityClass->GetDefaultObject(false);
    TStructArray<FGameplayAbilitySpec>& Items = ActivatableAbilities.Items;
    for (int32_t Index = 0; Index < Items.Num(); ++Index)
    {
        FGameplayAbilitySpec& Spec = Items[Index];
        if (Spec.Ability == AbilityDefault)
        {
            return &Spec;
        }
    }

    return nullptr;
}

void UAbilitySystemComponent::MarkAbilitySpecDirty(FGameplayAbilitySpec& Spec)
{
    Natives::UAbilitySystemComponent_MarkAbilitySpecDirty(this, Spec);
}

void FGameplayAbilitySpecHandle::GenerateNewHandle()
{
    Handle = (*Natives::FGameplayAbilitySpecHandle_GHandleID)++;
}

FGameplayAbilitySpecHandle UAbilitySystemComponent::GiveAbilityAndActivateOnce(const FGameplayAbilitySpec& Spec)
{
    FGameplayAbilitySpecHandle Handle;
    Natives::UAbilitySystemComponent_GiveAbilityAndActivateOnce(this, &Handle, Spec);
    return Handle;
}

FGameplayAbilitySpecHandle UAbilitySystemComponent::GiveAbility(const FGameplayAbilitySpec& Spec)
{
    // Like GiveAbilityAndActivateOnce, the member function returns its handle through a hidden result pointer.
    FGameplayAbilitySpecHandle Handle;
    if (Natives::UAbilitySystemComponent_GiveAbility)
    {
        using FGiveAbility = FGameplayAbilitySpecHandle* (*)(UAbilitySystemComponent* This, FGameplayAbilitySpecHandle* Result, const FGameplayAbilitySpec& Spec);
        reinterpret_cast<FGiveAbility>(Natives::UAbilitySystemComponent_GiveAbility)(this, &Handle, Spec);
    }
    return Handle;
}

bool UAbilitySystemComponent::TryActivateAbility(FGameplayAbilitySpecHandle AbilityToActivate)
{
    if (!FindAbilitySpecFromHandle(AbilityToActivate))
    {
        ABILITY_LOG(Warning, TEXT("TryActivateAbility called with invalid Handle"));
        return false;
    }

    // FPredictionKey's engine layout is longer than the members Core reflects: a default key in the build's size.
    UScriptStruct* PredictionKeyStruct = FPredictionKey::StaticStruct();
    std::vector<uint8_t> PredictionKeyStorage(PredictionKeyStruct->GetStructureSize(), 0);
    PredictionKeyStruct->InitializeStruct(PredictionKeyStorage.data());
    const bool bActivated = InternalTryActivateAbility(AbilityToActivate, *reinterpret_cast<const FPredictionKey*>(PredictionKeyStorage.data()));
    PredictionKeyStruct->DestroyStruct(PredictionKeyStorage.data());
    return bActivated;
}

bool UAbilitySystemComponent::InternalTryActivateAbility(FGameplayAbilitySpecHandle AbilityToActivate, const FPredictionKey& InPredictionKey, UGameplayAbility** OutInstancedAbility, void* OnGameplayAbilityEndedDelegate, const FGameplayEventData* TriggerEventData)
{
    UScriptStruct* PredictionKeyStruct = FPredictionKey::StaticStruct();
    std::vector<uint8_t> PredictionKeyCopy(PredictionKeyStruct->GetStructureSize(), 0);
    PredictionKeyStruct->CopyScriptStruct(PredictionKeyCopy.data(), &InPredictionKey);
    return Natives::UAbilitySystemComponent_InternalTryActivateAbility(this, AbilityToActivate, PredictionKeyCopy.data(), OutInstancedAbility, OnGameplayAbilityEndedDelegate, TriggerEventData);
}

bool UAbilitySystemComponent::TryActivateAbilitiesByTag(const FGameplayTagContainer& GameplayTagContainer, bool bAllowRemoteActivation)
{
    UFunction* Function = FindStaticFunction_TryActivateAbilitiesByTag();
    if (!Function)
    {
        return false;
    }
    Reflection::FParams Params(Function, true);
    if (!Params.Set(L"GameplayTagContainer", GameplayTagContainer) || !Params.Set(L"bAllowRemoteActivation", bAllowRemoteActivation) || !Params.Call(this))
    {
        return false;
    }
    return Params.Take<bool>(L"ReturnValue");
}

void UAbilitySystemComponent::CancelAbilities(const FGameplayTagContainer* WithTags, const FGameplayTagContainer* WithoutTags, UGameplayAbility* Ignore)
{
    if (Natives::UAbilitySystemComponent_CancelAbilities)
    {
        Natives::UAbilitySystemComponent_CancelAbilities(this, WithTags, WithoutTags, Ignore);
        return;
    }

    // The engine's walk of ActivatableAbilities (under its ability list lock): every active spec whose ability's tags pass
    // goes through CancelAbilitySpec, which cancels each of its instances on that instance's current activation. Core
    // takes the instances first and cancels them after the walk, since an ability that ends can take its spec with it
    // (GiveAbilityAndActivateOnce). A non-instanced ability is cancelled on its class default with the spec's activation,
    // which only the native does, so it is left alone here.
    std::vector<UGameplayAbility*> AbilitiesToCancel;
    TStructArray<FGameplayAbilitySpec>& Items = ActivatableAbilities.Items;
    for (int32_t Index = 0; Index < Items.Num(); ++Index)
    {
        FGameplayAbilitySpec& Spec = Items[Index];
        UGameplayAbility* const SpecAbility = Spec.Ability;
        const uint8_t ActiveCount = Spec.ActiveCount;
        if (ActiveCount == 0 || !SpecAbility)
        {
            continue;
        }

        const FGameplayTagContainer& AbilityTags = SpecAbility->AbilityTags;
        const bool WithTagPass = !WithTags || AbilityTags.HasAll(*WithTags);
        const bool WithoutTagPass = !WithoutTags || !AbilityTags.HasAny(*WithoutTags);
        if (!WithTagPass || !WithoutTagPass)
        {
            continue;
        }

        TArray<UGameplayAbility*>& ReplicatedInstances = Spec.ReplicatedInstances;
        TArray<UGameplayAbility*>& NonReplicatedInstances = Spec.NonReplicatedInstances;
        for (TArray<UGameplayAbility*>* Instances : { &ReplicatedInstances, &NonReplicatedInstances })
        {
            for (UGameplayAbility* InstanceAbility : *Instances)
            {
                if (InstanceAbility && InstanceAbility != Ignore)
                {
                    AbilitiesToCancel.push_back(InstanceAbility);
                }
            }
        }
    }

    for (UGameplayAbility* InstanceAbility : AbilitiesToCancel)
    {
        InstanceAbility->K2_CancelAbility();
    }
}

void UAbilitySystemComponent::ClientActivateAbilityFailed(FGameplayAbilitySpecHandle AbilityToActivate, int16_t PredictionKey)
{
    Reflection::FParams Params(StaticFunction_ClientActivateAbilityFailed());
    Params.Set(L"AbilityToActivate", AbilityToActivate);
    Params.Set(L"PredictionKey", PredictionKey);
    Params.Call(this);
}

bool UAbilitySystemComponent::RemoveActiveGameplayEffectBySourceEffect(UClass* GameplayEffect, UAbilitySystemComponent* InstigatorAbilitySystemComponent, int32_t StacksToRemove)
{
    UFunction* Function = FindStaticFunction_RemoveActiveGameplayEffectBySourceEffect();
    if (!Function)
    {
        return false;
    }
    Reflection::FParams Params(Function);
    Params.Set(L"GameplayEffect", GameplayEffect);
    Params.Set(L"InstigatorAbilitySystemComponent", InstigatorAbilitySystemComponent);
    Params.Set(L"StacksToRemove", StacksToRemove);
    return Params.Call(this);
}

void UAbilitySystemComponent::IncrementAbilityListLock()
{
    Natives::UAbilitySystemComponent_IncrementAbilityListLock(this);
}

void UAbilitySystemComponent::DecrementAbilityListLock()
{
    Natives::UAbilitySystemComponent_DecrementAbilityListLock(this);
}

void UAbilitySystemComponent::InternalServerTryActiveAbility(FGameplayAbilitySpecHandle Handle, bool InputPressed, const FPredictionKey& PredictionKey, const FGameplayEventData* TriggerEventData)
{
#if WITH_SERVER_CODE
    ABILITYLIST_SCOPE_LOCK();
    FGameplayAbilitySpec* Spec = FindAbilitySpecFromHandle(Handle);
    if (!Spec)
    {
        ABILITY_LOG(Display, TEXT("InternalServerTryActiveAbility. Rejecting ClientActivation of ability with invalid SpecHandle!"));
        ClientActivateAbilityFailed(Handle, PredictionKey.Current);
        return;
    }

    FScopedPredictionWindow ScopedPredictionWindow(this, PredictionKey);

    UGameplayAbility* InstancedAbility = nullptr;
    Spec->InputPressed = true;

    if (!InternalTryActivateAbility(Handle, PredictionKey, &InstancedAbility, nullptr, TriggerEventData))
    {
        ABILITY_LOG(Display, TEXT("InternalServerTryActiveAbility. Rejecting ClientActivation of %s with PredictionKey %s. InternalTryActivateAbility failed: %s"), *GetNameSafe(Spec->Ability), *PredictionKey.ToString(), *DescribeActivationFailure(*Spec));
        ClientActivateAbilityFailed(Handle, PredictionKey.Current);
        Spec->InputPressed = false;
        MarkAbilitySpecDirty(*Spec);
    }
#endif
}

FString UAbilitySystemComponent::DescribeActivationFailure(const FGameplayAbilitySpec& Spec) const
{
    // The native's checks, in its order: a valid owner and avatar, an avatar that isn't a simulated proxy, the user
    // activation inhibit, the spec's input not blocked, then the ability's owner tag requirements.
    std::wstring Description;
    AActor* const Owner = OwnerActor;
    Description += L"owner " + GetNameSafe(Owner).ToStdString();
    if (Member_AvatarActor.IsAvailable())
    {
        AActor* const Avatar = AvatarActor;
        Description += L", avatar " + GetNameSafe(Avatar).ToStdString();
        if (Avatar)
        {
            Description += L" (role " + std::to_wstring(static_cast<int32_t>(static_cast<uint8_t>(Avatar->Role))) + L")";
        }
    }
    if (Member_UserAbilityActivationInhibited.IsAvailable() && UserAbilityActivationInhibited)
    {
        Description += L", user ability activation inhibited";
    }
    if (Member_BlockedAbilityBindings.IsAvailable())
    {
        const TArray<uint8_t>& Bindings = BlockedAbilityBindings;
        if (Spec.InputID >= 0 && Spec.InputID < Bindings.Num() && Bindings[Spec.InputID] > 0)
        {
            Description += L", input " + std::to_wstring(Spec.InputID) + L" blocked";
        }
    }
    if (UGameplayAbility* const Ability = Spec.Ability)
    {
        Description += L", instancing " + std::to_wstring(Ability->GetInstancingPolicyValue()) + L", net execution " + std::to_wstring(Ability->GetNetExecutionPolicyValue())
            + L", " + std::to_wstring(Spec.ReplicatedInstances.Num() + Spec.NonReplicatedInstances.Num()) + L" instances";
        FGameplayTagContainer OwnedTags;
        GetOwnedGameplayTags(OwnedTags);
        Description += L";" + Ability->DescribeTagRequirements(OwnedTags);
    }
    return FString(Description.c_str());
}

void UAbilitySystemComponent::HookVTables()
{
    if (!Finder::ResolveAbilitySystemLive())
    {
        return;
    }

    const int32_t ActivateIndex = Natives::UAbilitySystemComponent_InternalServerTryActiveAbilityIndex;
    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Class = *It;
        UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
        void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
        if (VTable && VTable[ActivateIndex] == Natives::SharedEmptyFunction)
        {
            Hooks::SwapVTable(L"UAbilitySystemComponent::InternalServerTryActiveAbility", VTable, ActivateIndex, reinterpret_cast<void*>(&InternalServerTryActiveAbilityHK));
        }
    }
}

void UAbilitySystemComponent::InternalServerTryActiveAbilityHK(UAbilitySystemComponent* This, FGameplayAbilitySpecHandle Handle, bool InputPressed, const FPredictionKey& PredictionKey, const FGameplayEventData* TriggerEventData)
{
    This->InternalServerTryActiveAbility(Handle, InputPressed, PredictionKey, TriggerEventData);
}

void UAbilitySystemComponent::ServerSetInputReleased(FGameplayAbilitySpecHandle AbilityHandle)
{
    Reflection::FParams Params(StaticFunction_ServerSetInputReleased(), true);
    Params.Set(L"AbilityHandle", AbilityHandle);
    Params.Call(this);
}
