#pragma once

#include "Containers/Array.h"
#include "Items/FortItem.h"
#include "UObject/AssetPtr.h"

class UCustomCharacterPart;
class UFortAbilityKit;
class UFortHeroType;

class UFortHeroSpecialization : public UObject
{
    DefineUnrealClass(UFortHeroSpecialization, FortniteGame)

public:
    DefineUProperty(TArray<TAssetPtr<UCustomCharacterPart>>, CharacterParts)
};

class UFortHero : public UFortItem
{
    DefineUnrealClass(UFortHero, FortniteGame)
    DefineUFunction(GetHeroTypeBP)

public:
    DefineUProperty(TArray<UCustomCharacterPart*>, CharacterParts)
    DefineUProperty(TArray<UFortHeroSpecialization*>, Specializations)
    DefineUProperty(TArray<UFortAbilityKit*>, SpecializationAbilityKits)

    UFortHeroType* GetHeroTypeBP() const;
};
