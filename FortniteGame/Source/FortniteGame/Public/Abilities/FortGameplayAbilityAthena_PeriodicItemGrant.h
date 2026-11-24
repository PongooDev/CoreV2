#pragma once

#include "Abilities/FortGameplayAbility.h"
#include "Containers/ScriptMap.h"
#include "Engine/EngineTypes.h"
#include "GameplayEffect.h"
#include "UObject/Stack.h"

class AFortPlayerController;
class UFortWorldItemDefinition;

// One item a periodic grant hands out: AmountToGive each time, while the owner has fewer than MaxAmount.
struct FActiveItemGrantInfo
{
    DefineUnrealStruct(FActiveItemGrantInfo, FortniteGame)

public:
    DefineUProperty(UFortWorldItemDefinition*, Item)
    DefineUStructProperty(FScalableFloat, AmountToGive)
    DefineUStructProperty(FScalableFloat, MaxAmount)
};

// 8.40 (CL-6005771) added it: a passive ability that gives its owner items on timers while active. Food Fight: Deep
// Fried's GA_Lava (AS_Lava, from BarrierModifier_B_Lava) calls StartItemAwardTimers from K2_ActivateAbility and
// StopItemAwardTimers from K2_OnEndAbility, for wood, stone and metal every Lava.*Tick.Time seconds.
class UFortGameplayAbilityAthena_PeriodicItemGrant : public UFortGameplayAbility
{
    DefineUnrealClass(UFortGameplayAbilityAthena_PeriodicItemGrant, FortniteGame)
    DefineUFunction(StartItemAwardTimers)
    DefineUFunction(StopItemAwardTimers)

public:
    // TMap<FActiveItemGrantInfo, FScalableFloat>: each item and its interval.
    DefineUProperty(FScriptMap, ItemsToGrant)
    DefineUProperty(TArray<FTimerHandle>, ActiveTimers)
    DefineUProperty(AFortPlayerController*, OwnerControllerCachedValue)

    void StartItemAwardTimers();
    void StopItemAwardTimers();

    DECLARE_FUNCTION(execStartItemAwardTimers);
    DECLARE_FUNCTION(execStopItemAwardTimers);

    static void HookNatives();

private:
    // Core's name: a timer's grant of ItemsToGrant's pair at Index.
    void AwardItem(int32_t Index);
    FScriptMapLayout GetItemsToGrantLayout() const;
};
