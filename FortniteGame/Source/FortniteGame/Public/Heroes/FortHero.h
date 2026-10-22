#pragma once

#include "Containers/Array.h"
#include "Items/FortItem.h"

class UCustomCharacterPart;
class UFortAbilityKit;
class UFortHeroType;

class UFortHero : public UFortItem
{
    DefineUnrealClass(UFortHero, FortniteGame)
    DefineUFunction(GetHeroTypeBP)

public:
    DefineUProperty(TArray<UCustomCharacterPart*>, CharacterParts)
    DefineUProperty(TArray<UFortAbilityKit*>, SpecializationAbilityKits)

    UFortHeroType* GetHeroTypeBP() const;
};
