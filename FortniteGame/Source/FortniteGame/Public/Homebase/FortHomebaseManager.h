#pragma once

#include "Containers/Array.h"
#include "Items/FortItem.h"
#include "StructArray.h"
#include "UObject/Object.h"

class IAbilitySystemInterface;
class UFortAbilityKit;
class UFortMcpProfileCampaign;

class UFortHomebaseManager : public UObject
{
    DefineUnrealClass(UFortHomebaseManager, FortniteGame)

public:
    void GetSelectableGadgetItems(const UFortMcpProfileCampaign* AccountProfile, IAbilitySystemInterface* AbilitySystem, TStructArray<FFortItemEntry>& OutItems) const;
    void GetOwnedNodeAbilityKits(const UFortMcpProfileCampaign* AccountProfile, TArray<UFortAbilityKit*>& OutKits) const;
};
