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
    EquipGenericPlayerAbilitySet(AbilitySystem, OutAbilitySetHandles);
}

void UFortGlobals::ApplyHeroEffectsAndAbilities(UFortCampaignHeroLoadoutItem* HeroLoadout, UFortHero* Hero, IAbilitySystemInterface* AbilitySystem, TArray<FActiveGameplayEffectHandle>& OutEffectHandles,
    FScriptArray& OutAbilitySetHandles)
{
    ApplyHeroEffectsAndAbilitiesWithLoadoutOG(HeroLoadout, Hero, AbilitySystem, OutEffectHandles, OutAbilitySetHandles);
    EquipGenericPlayerAbilitySet(AbilitySystem, OutAbilitySetHandles);
}

void UFortGlobals::EquipGenericPlayerAbilitySet(IAbilitySystemInterface* AbilitySystem, FScriptArray& OutAbilitySetHandles)
{
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
    // 7.00 (CL-4629139) takes the hero loadout first: the detour has to read the game's arguments where they are (#472).
    if (Natives::UFortGlobals_ApplyHeroEffectsAndAbilitiesTakesHeroLoadout)
    {
        void (*const Detour)(UFortCampaignHeroLoadoutItem*, UFortHero*, IAbilitySystemInterface*, TArray<FActiveGameplayEffectHandle>&, FScriptArray&) = &ApplyHeroEffectsAndAbilities;
        Hooks::Detour(L"UFortGlobals::ApplyHeroEffectsAndAbilities", reinterpret_cast<void*>(Natives::UFortGlobals_ApplyHeroEffectsAndAbilities), Detour, &ApplyHeroEffectsAndAbilitiesWithLoadoutOG);
        return;
    }
    void (*const Detour)(UFortHero*, IAbilitySystemInterface*, TArray<FActiveGameplayEffectHandle>&, FScriptArray&) = &ApplyHeroEffectsAndAbilities;
    Hooks::Detour(L"UFortGlobals::ApplyHeroEffectsAndAbilities", reinterpret_cast<void*>(Natives::UFortGlobals_ApplyHeroEffectsAndAbilities), Detour, &ApplyHeroEffectsAndAbilitiesOG);
}
