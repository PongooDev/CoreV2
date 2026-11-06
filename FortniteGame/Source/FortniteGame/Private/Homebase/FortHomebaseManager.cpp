#include "pch.h"
#include "Homebase/FortHomebaseManager.h"
#include "Finder.h"
#include "Natives.h"

void UFortHomebaseManager::GetSelectableGadgetItems(const UFortMcpProfileCampaign* AccountProfile, IAbilitySystemInterface* AbilitySystem, TStructArray<FFortItemEntry>& OutItems) const
{
    if (Finder::ResolveHomebaseGadgetsLive())
    {
        Natives::UFortHomebaseManager_GetSelectableGadgetItems(this, AccountProfile, AbilitySystem, OutItems);
    }
}

void UFortHomebaseManager::GetOwnedNodeAbilityKits(const UFortMcpProfileCampaign* AccountProfile, TArray<UFortAbilityKit*>& OutKits) const
{
    if (Finder::ResolveHomebaseGadgetsLive())
    {
        Natives::UFortHomebaseManager_GetOwnedNodeAbilityKits(this, AccountProfile, OutKits);
    }
}
