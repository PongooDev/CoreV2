#include "pch.h"
#include "Homebase/FortHomebaseManager.h"
#include "Engine/DataTable.h"
#include "Finder.h"
#include "Items/FortItemDefinition.h"
#include "Logging/LogMacros.h"
#include "Natives.h"

DEFINE_LOG_CATEGORY_STATIC(LogHomebase, Log, All);

TArray<FName> UFortHomebaseManager::GetOwnedNodes(const UFortMcpProfileCampaign* McpProfile) const
{
    TArray<FName> OwnedNodes;
    if (Finder::ResolveHomebaseGadgetsLive() && Natives::UFortHomebaseManager_GetOwnedNodes)
    {
        Natives::UFortHomebaseManager_GetOwnedNodes(this, &OwnedNodes, McpProfile);
    }
    return OwnedNodes;
}

void UFortHomebaseManager::GetSelectableGadgetItems(const UFortMcpProfileCampaign* AccountProfile, IAbilitySystemInterface* AbilitySystem, TStructArray<FFortItemEntry>& OutItems) const
{
    if (Finder::ResolveHomebaseGadgetsLive())
    {
        Natives::UFortHomebaseManager_GetSelectableGadgetItems(this, AccountProfile, AbilitySystem, OutItems);
    }
}

void UFortHomebaseManager::GetOwnedNodeAbilityKits(const UFortMcpProfileCampaign* AccountProfile, TArray<UFortAbilityKit*>& OutKits) const
{
    if (!AccountProfile || !Finder::ResolveHomebaseGadgetsLive())
    {
        return;
    }
    if (Natives::UFortHomebaseManager_GetOwnedNodeAbilityKits)
    {
        Natives::UFortHomebaseManager_GetOwnedNodeAbilityKits(this, AccountProfile, OutKits);
        return;
    }

    // 6.30+: no skill tree. The kits are the account's HomebaseNode items' (the Upgrades page's nodes), each one's
    // levels' AbilityKit up to the item's level, as GetSelectableGadgetItems gathers them (6.31 0xFB0631, 0xFB067D).
    if (Natives::UFortHomebaseNodeItem_GetAbilityKits && Natives::UFortMcpProfile_GetHomebaseNodeItemsByType)
    {
        TArray<UObject*> NodeItems;
        Natives::UFortMcpProfile_GetHomebaseNodeItemsByType(AccountProfile, EFortItemType::GetHomebaseNode(), NodeItems);
        for (UObject* NodeItem : NodeItems)
        {
            TArray<UFortAbilityKit*> NodeKits;
            Natives::UFortHomebaseNodeItem_GetAbilityKits(NodeItem, &NodeKits);
            for (UFortAbilityKit* Kit : NodeKits)
            {
                OutKits.Add(Kit);
            }
        }
        return;
    }

    // 5.0 inlines this into GetSelectableGadgetItems; the same loop as 4.5's (0xCCA4D0).
    if (!HasHomebaseNodeDataTable())
    {
        static bool bLogged = false;
        if (!bLogged)
        {
            bLogged = true;
            UE_LOG(LogHomebase, Warning, TEXT("This build has no skill tree (no HomebaseNodeDataTable): no gadgets come from owned nodes"));
        }
        return;
    }
    for (const FName& NodeId : GetOwnedNodes(AccountProfile))
    {
        if (NodeId.IsNone())
        {
            continue;
        }
        UDataTable* NodeTable = HomebaseNodeDataTable;
        if (!NodeTable)
        {
            UE_LOG(LogHomebase, Warning, TEXT("Missing HomebaseNodeDataTable object"));
            continue;
        }
        const FHomebaseNode* Node = NodeTable->FindRow<FHomebaseNode>(NodeId, L"Finding homebase node row", true);
        if (Node && Node->AbilityKit)
        {
            OutKits.Add(Node->AbilityKit);
        }
    }
}
