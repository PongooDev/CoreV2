#pragma once

#include "Containers/Array.h"
#include "Items/FortItem.h"
#include "StructArray.h"
#include "UObject/AssetPtr.h"

class UCustomCharacterPart;
class UFortAbilityKit;
class UFortHeroType;

// One perk or ability of a hero specialization: the kit it grants (and the earlier one it replaces) from a hero level.
struct FFortSpecializationSlot
{
    DefineUnrealStruct(FFortSpecializationSlot, FortniteGame)
    DefineUProperty(TAssetPtr<UFortAbilityKit>, GrantedAbilityKit)
    DefineUProperty(TAssetPtr<UFortAbilityKit>, RemovedAbilityKit)
    DefineUProperty(int32_t, MinimumHeroLevel)
};

class UFortHeroSpecialization : public UObject
{
    DefineUnrealClass(UFortHeroSpecialization, FortniteGame)

public:
    DefineUProperty(TArray<TAssetPtr<UCustomCharacterPart>>, CharacterParts)
    DefineUStructArrayProperty(FFortSpecializationSlot, SpecializationSlots)
    DefineUStructArrayProperty(FFortSpecializationSlot, TacticalAbilitySlots)
};

class UFortHero : public UFortItem
{
    DefineUnrealClass(UFortHero, FortniteGame)
    DefineUFunction(GetHeroTypeBP)

public:
    DefineUProperty(TArray<UCustomCharacterPart*>, CharacterParts)
    DefineUProperty(TArray<UFortHeroSpecialization*>, Specializations)
    DefineUProperty(TArray<UFortAbilityKit*>, SpecializationAbilityKits)
    // UFortWorker's: the EFortCustomGender the hero plays as (an int32 here, a byte on its hero type).
    DefineUProperty(int32_t, Gender)

    UFortHeroType* GetHeroTypeBP() const;

    static bool HasGender() { return Member_Gender.IsAvailable(); }
};
