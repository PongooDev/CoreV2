#pragma once

#include "Abilities/FortAbilitySet.h"
#include "Containers/Array.h"
#include "StructArray.h"

class UFortGadgetItemDefinition;
class UFortItemDefinition;

class EFortReplenishmentType
{
    DefineUnrealEnum(EFortReplenishmentType, FortniteGame)
    DefineUEnumValue(Restricted)
    DefineUEnumValue(ClampMin)
    DefineUEnumValue(Add)
    DefineUEnumValue(Ability)
};
// does something
struct FAbilityKitItem
{
    DefineUnrealStruct(FAbilityKitItem, FortniteGame)
    DefineUProperty(UFortItemDefinition*, Item)
    DefineUProperty(int32_t, Quantity)
    DefineUProperty(EFortReplenishmentType, Replenishment)
};

class UFortAbilityKit : public UFortAbilitySet
{
    DefineUnrealClass(UFortAbilityKit, FortniteGame)

public:
    DefineUProperty(TArray<UFortGadgetItemDefinition*>, Gadgets)
    DefineUStructArrayProperty(FAbilityKitItem, Items)
};
