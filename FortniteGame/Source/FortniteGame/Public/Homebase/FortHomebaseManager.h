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
    // 6.30 (CL-4573096) replaced the skill tree with the Upgrades page: the manager has no node table, and FHomebaseNode
    // is gone.
    static bool HasHomebaseNodeDataTable() { return Member_HomebaseNodeDataTable.IsAvailable(); }

    TArray<FName> GetOwnedNodes(const UFortMcpProfileCampaign* McpProfile) const;
    void GetSelectableGadgetItems(const UFortMcpProfileCampaign* AccountProfile, IAbilitySystemInterface* AbilitySystem, TStructArray<FFortItemEntry>& OutItems) const;
    void GetOwnedNodeAbilityKits(const UFortMcpProfileCampaign* AccountProfile, TArray<UFortAbilityKit*>& OutKits) const;
};
