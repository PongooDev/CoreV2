#include "pch.h"
#include "AbilitySystemComponent.h"
#include "Abilities/GameplayAbility.h"
#include "Finder.h"
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
        ABILITY_LOG(Display, TEXT("InternalServerTryActiveAbility. Rejecting ClientActivation of %s with PredictionKey %s. InternalTryActivateAbility failed"), *GetNameSafe(Spec->Ability), *PredictionKey.ToString());
        ClientActivateAbilityFailed(Handle, PredictionKey.Current);
        Spec->InputPressed = false;
        MarkAbilitySpecDirty(*Spec);
    }
#endif
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
