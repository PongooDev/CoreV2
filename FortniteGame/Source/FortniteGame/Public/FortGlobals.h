#pragma once

#include "Containers/Array.h"
#include "Containers/ScriptArray.h"
#include "UObject/Object.h"

class IAbilitySystemInterface;
class UFortHero;
struct FActiveGameplayEffectHandle;

class UFortGlobals : public UObject
{
    DefineUnrealClass(UFortGlobals, FortniteGame)

public:
    static void ApplyHeroEffectsAndAbilities(UFortHero* Hero, IAbilitySystemInterface* AbilitySystem, TArray<FActiveGameplayEffectHandle>& OutEffectHandles, FScriptArray& OutAbilitySetHandles);

    static void Hook();

private:
    static inline void (*ApplyHeroEffectsAndAbilitiesOG)(UFortHero* Hero, IAbilitySystemInterface* AbilitySystem, TArray<FActiveGameplayEffectHandle>& OutEffectHandles, FScriptArray& OutAbilitySetHandles) = nullptr;
};
