#pragma once

#include "GameplayTagContainer.h"
#include "Items/FortItemDefinition.h"
#include "StructArray.h"
#include "UObject/AssetPtr.h"

class UFortAbilityKit;

struct FHeroAbilityKit
{
    DefineUnrealStruct(FHeroAbilityKit, FortniteGame)
    DefineUProperty(TAssetPtr<UFortAbilityKit>, InherentAbilityKit)
    DefineUProperty(FGameplayTagContainer, RequiredGPTags)
};

class UFortHeroType : public UFortItemDefinition
{
    DefineUnrealClass(UFortHeroType, FortniteGame)

public:
    DefineUStructArrayProperty(FHeroAbilityKit, AbilityKits)
};
