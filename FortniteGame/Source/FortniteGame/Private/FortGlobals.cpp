#include "pch.h"
#include "FortGlobals.h"
#include "Abilities/FortAbilitySet.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemInterface.h"
#include "FortAssetManager.h"
#include "FortGameData.h"
#include "FortKismetLibrary.h"
#include "GameFramework/Actor.h"
#include "Hooks.h"
#include "Natives.h"
#include "UObject/Class.h"

void UFortGlobals::ApplyHeroEffectsAndAbilities(UFortHero* Hero, IAbilitySystemInterface* AbilitySystem, TArray<FActiveGameplayEffectHandle>& OutEffectHandles, FScriptArray& OutAbilitySetHandles)
{
    ApplyHeroEffectsAndAbilitiesOG(Hero, AbilitySystem, OutEffectHandles, OutAbilitySetHandles);

#if WITH_SERVER_CODE
    UAbilitySystemComponent* AbilitySystemComponent = AbilitySystem ? AbilitySystem->GetAbilitySystemComponent() : nullptr;
    AActor* OwnerActor = AbilitySystemComponent ? AbilitySystemComponent->OwnerActor : nullptr;
    if (!OwnerActor || OwnerActor->Role != ROLE_Authority)
    {
        return;
    }

    UFortGameData* GameData = UFortAssetManager::Get().GetGameData();
    UFortAbilitySet* GenericPlayerAbilitySet = GameData ? GameData->GenericPlayerAbilitySet.LoadSynchronous() : nullptr;
    if (!GenericPlayerAbilitySet)
    {
        return;
    }

    const int32_t HandleSize = FFortAbilitySetHandle::StaticStruct()->GetStructureSize();
    const int32_t Index = OutAbilitySetHandles.AddZeroed(1, HandleSize);
    FFortAbilitySetHandle& Handle = *reinterpret_cast<FFortAbilitySetHandle*>(static_cast<uint8_t*>(OutAbilitySetHandles.GetData()) + static_cast<size_t>(Index) * HandleSize);
    UFortKismetLibrary::EquipFortAbilitySet(TScriptInterface<IAbilitySystemInterface>(AbilitySystem->_getUObject(), AbilitySystem), GenericPlayerAbilitySet, nullptr, Handle);
#endif
}

void UFortGlobals::Hook()
{
    Hooks::Detour(L"UFortGlobals::ApplyHeroEffectsAndAbilities", reinterpret_cast<void*>(Natives::UFortGlobals_ApplyHeroEffectsAndAbilities), &ApplyHeroEffectsAndAbilities, &ApplyHeroEffectsAndAbilitiesOG);
}
