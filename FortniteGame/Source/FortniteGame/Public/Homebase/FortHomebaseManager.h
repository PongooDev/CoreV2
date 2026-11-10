#pragma once

#include "Containers/Array.h"
#include "Items/FortItem.h"
#include "StructArray.h"
#include "UObject/Object.h"

class IAbilitySystemInterface;
class UDataTable;
class UFortAbilityKit;
class UFortMcpProfileCampaign;

// A HomebaseNodeDataTable row (a skill tree node).
struct FHomebaseNode
{
    DefineUnrealStruct(FHomebaseNode, FortniteGame)

public:
    DefineUProperty(UFortAbilityKit*, AbilityKit)
};

class UFortHomebaseManager : public UObject
{
    DefineUnrealClass(UFortHomebaseManager, FortniteGame)

public:
    DefineUProperty(UDataTable*, HomebaseNodeDataTable)

    TArray<FName> GetOwnedNodes(const UFortMcpProfileCampaign* McpProfile) const;
    void GetSelectableGadgetItems(const UFortMcpProfileCampaign* AccountProfile, IAbilitySystemInterface* AbilitySystem, TStructArray<FFortItemEntry>& OutItems) const;
    void GetOwnedNodeAbilityKits(const UFortMcpProfileCampaign* AccountProfile, TArray<UFortAbilityKit*>& OutKits) const;
};
