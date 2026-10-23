#pragma once

#include "Containers/Array.h"
#include "Items/FortItem.h"
#include "StructArray.h"
#include "UObject/Object.h"

class IAbilitySystemInterface;
class UFortAbilityKit;
class UFortMcpProfileAccount;

class UFortHomebaseManager : public UObject
{
    DefineUnrealClass(UFortHomebaseManager, FortniteGame)

public:
    void GetSelectableGadgetItems(const UFortMcpProfileAccount* AccountProfile, IAbilitySystemInterface* AbilitySystem, TStructArray<FFortItemEntry>& OutItems) const;
    void GetOwnedNodeAbilityKits(const UFortMcpProfileAccount* AccountProfile, TArray<UFortAbilityKit*>& OutKits) const;
};
