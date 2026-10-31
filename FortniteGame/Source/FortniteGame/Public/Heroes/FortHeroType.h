#pragma once

#include "GameplayTagContainer.h"
#include "Items/FortItemDefinition.h"
#include "StructArray.h"
#include "UObject/AssetPtr.h"

class UFortAbilityKit;
class UFortHeroSpecialization;
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
    // The hero's specializations (HS_*): its character parts and its perks' and abilities' kits.
    DefineUProperty(TArray<TAssetPtr<UFortHeroSpecialization>>, Specializations)
    // UFortWorkerType's EFortCustomGender (1 female, 2 male).
    DefineUProperty(uint8_t, Gender)
    // UFortAccountItemDefinition's: the highest level a hero of this type reaches.
    DefineUProperty(int32_t, MaxLevel)

    // What the hero's CombinedStatGEs add to Attribute as a fixed amount.
    float GetCombinedStatBonus(const UProperty* Attribute) const;

    static bool HasGender() { return Member_Gender.IsAvailable(); }
    static bool HasMaxLevel() { return Member_MaxLevel.IsAvailable(); }
};
