#include "pch.h"
#include "Homebase/FortHomebaseManager.h"
#include "Engine/DataTable.h"
#include "Finder.h"
#include "Logging/LogMacros.h"
#include "Natives.h"

DEFINE_LOG_CATEGORY_STATIC(LogHomebase, Log, All);

TArray<FName> UFortHomebaseManager::GetOwnedNodes(const UFortMcpProfileCampaign* McpProfile) const
{
    TArray<FName> OwnedNodes;
    if (Finder::ResolveHomebaseGadgetsLive())
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

    // 5.0 inlines this into GetSelectableGadgetItems; the same loop as 4.5's (0xCCA4D0).
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
