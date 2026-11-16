#pragma once

#include "Containers/Array.h"
#include "Containers/ScriptArray.h"
#include "UObject/Object.h"

class IAbilitySystemInterface;
class UFortCampaignHeroLoadoutItem;
class UFortHero;
struct FActiveGameplayEffectHandle;

class UFortGlobals : public UObject
{
    DefineUnrealClass(UFortGlobals, FortniteGame)

public:
    static void ApplyHeroEffectsAndAbilities(UFortHero* Hero, IAbilitySystemInterface* AbilitySystem, TArray<FActiveGameplayEffectHandle>& OutEffectHandles, FScriptArray& OutAbilitySetHandles);
    // 7.00 (CL-4629139) on: the account's hero loadout comes first (null when the player has none).
    static void ApplyHeroEffectsAndAbilities(UFortCampaignHeroLoadoutItem* HeroLoadout, UFortHero* Hero, IAbilitySystemInterface* AbilitySystem, TArray<FActiveGameplayEffectHandle>& OutEffectHandles,
        FScriptArray& OutAbilitySetHandles);

    static void Hook();

private:
    // Core's step after the game's: the generic player ability set for a server-side ability system (both signatures).
    static void EquipGenericPlayerAbilitySet(IAbilitySystemInterface* AbilitySystem, FScriptArray& OutAbilitySetHandles);

    static inline void (*ApplyHeroEffectsAndAbilitiesOG)(UFortHero* Hero, IAbilitySystemInterface* AbilitySystem, TArray<FActiveGameplayEffectHandle>& OutEffectHandles, FScriptArray& OutAbilitySetHandles) = nullptr;
    static inline void (*ApplyHeroEffectsAndAbilitiesWithLoadoutOG)(UFortCampaignHeroLoadoutItem* HeroLoadout, UFortHero* Hero, IAbilitySystemInterface* AbilitySystem,
        TArray<FActiveGameplayEffectHandle>& OutEffectHandles, FScriptArray& OutAbilitySetHandles) = nullptr;
};
