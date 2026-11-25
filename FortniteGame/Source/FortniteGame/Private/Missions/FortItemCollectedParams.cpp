#include "pch.h"
#include "Missions/FortItemCollectedParams.h"
#include "Missions/FortMissionLibrary.h"
#include "Items/FortItemDefinition.h"
#include "Player/FortPlayerController.h"
#include "Quests/FortQuestManager.h"
#include "ReflectionParams.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"

UFortItemCollectedParams* UFortItemCollectedParams::SetParams(UFortWorldItemDefinition* CollectedItemDefinition, AFortPlayerController* CollectedBy, int32_t AmountCollected)
{
    Reflection::FParams Params(StaticFunction_SetParams());
    Params.Set(L"_CollectedItemDefinition", CollectedItemDefinition);
    Params.Set(L"_CollectedBy", CollectedBy);
    Params.Set(L"_AmountCollected", AmountCollected);
    return Params.Call(this) ? Params.Take<UFortItemCollectedParams*>(L"ThisParam") : nullptr;
}

void UFortItemCollectedParams::SendItemCollected(AFortPlayerController* CollectedBy, UFortWorldItemDefinition* CollectedItemDefinition, int32_t AmountCollected)
{
#if WITH_SERVER_CODE
    if (!CollectedBy || !CollectedItemDefinition || AmountCollected <= 0)
    {
        return;
    }

    UObject* TransientPackage = StaticFindObject(nullptr, nullptr, L"/Engine/Transient");
    UFortItemCollectedParams* Params = NewObject<UFortItemCollectedParams>(TransientPackage, StaticClass());
    if (Params)
    {
        Params->SetParams(CollectedItemDefinition, CollectedBy, AmountCollected);
        UFortMissionLibrary::ExecuteMissionEvent(CollectedBy, FName(L"ItemCollected", FNAME_Find), Params);
        // MissionItemCollected is the mission manager's other collect event name (9.00 registers it at 0x13B7A0 beside
        // ItemCollected's 0x13B400; 8.51 0x12EEC0), with the same UFortItemCollectedParams. A mission's handler is
        // its HandleMissionEvent_<event name> (9.00 0x1566500), and the reactive harvest quests
        // (Mission_GenericHarvestingItemDrop_Parent: Canny Crude's oil samples and the other GatherCollect quests) only
        // have HandleMissionEvent_MissionItemCollected, which matches the item against their QuestWorldItemDefinition and
        // sends the quest's Custom stat. No code in 8.51 or 9.00 sends it (the sender is stripped), so the quest stayed
        // at 0 (#666). Builds without the name skip it.
        const FName MissionItemCollected(L"MissionItemCollected", FNAME_Find);
        if (!MissionItemCollected.IsNone())
        {
            UFortMissionLibrary::ExecuteMissionEvent(CollectedBy, MissionItemCollected, Params);
        }
    }
    // The Collect event goes to the current sub game's quest manager, as the game's own senders send theirs: Battle
    // Royale's challenges count it there (6.00's "Pickup a Legendary Item in different matches", collect_legendary_rarity_item:
    // Collect with the item's Rarity.SuperRare; Season 3's collect_building_resources_*). It went to Save the World's
    // quest manager in every mode, so no Battle Royale Collect objective ever moved (#407).
    // A building material's Collect is the harvest's: ABuildingSMActor::AttemptSpawnResources sends it as the pickaxe
    // hit gains the materials. Materials picked up or given ("Harvest Stone building resources with a pickaxe",
    // collect_building_resources_stone: Collect, target Building.Resource.Stone, which StoneItemData's own tags carry)
    // don't count (#652).
    if (Cast<UFortResourceItemDefinition>(CollectedItemDefinition))
    {
        return;
    }
    if (UFortQuestManager* QuestManager = CollectedBy->GetQuestManager(ESubGame::GetInvalid()))
    {
        QuestManager->SendItemStatEvent(EFortQuestObjectiveStatEvent::GetCollect(), CollectedItemDefinition, AmountCollected);
    }
#endif
}
