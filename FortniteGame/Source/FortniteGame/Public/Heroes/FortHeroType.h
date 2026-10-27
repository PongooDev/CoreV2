#pragma once

#include "GameplayTagContainer.h"
#include "Items/FortItemDefinition.h"
#include "StructArray.h"
#include "UObject/AssetPtr.h"

class UFortAbilityKit;
class UGameplayEffect;
class UProperty;

struct FHeroAbilityKit
{
    DefineUnrealStruct(FHeroAbilityKit, FortniteGame)
    DefineUProperty(TAssetPtr<UFortAbilityKit>, InherentAbilityKit)
    DefineUProperty(FGameplayTagContainer, RequiredGPTags)
};

struct FGameplayEffectApplicationInfo
{
    DefineUnrealStruct(FGameplayEffectApplicationInfo, FortniteGame)
    DefineUProperty(TAssetSubclassOf<UGameplayEffect>, GameplayEffect)
};

class UFortHeroType : public UFortItemDefinition
{
    DefineUnrealClass(UFortHeroType, FortniteGame)

public:
    DefineUStructArrayProperty(FHeroAbilityKit, AbilityKits)
    DefineUStructArrayProperty(FGameplayEffectApplicationInfo, CombinedStatGEs)

    // What the hero's CombinedStatGEs add to Attribute as a fixed amount.
    float GetCombinedStatBonus(const UProperty* Attribute) const;
};
