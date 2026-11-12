#include "pch.h"
#include "Missions/FortItemCollectedParams.h"
#include "Missions/FortMissionLibrary.h"
#include "Items/FortItemDefinition.h"
#include "Player/FortPlayerController.h"
#include "Quests/FortQuestManager.h"
#include "ReflectionParams.h"
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
    }
    // The Collect event goes to the current sub game's quest manager, as the game's own senders send theirs: Battle
    // Royale's challenges count it there (6.00's "Pickup a Legendary Item in different matches", collect_legendary_rarity_item:
    // Collect with the item's Rarity.SuperRare; Season 3's collect_building_resources_*). It went to Save the World's
    // quest manager in every mode, so no Battle Royale Collect objective ever moved (#407).
    if (UFortQuestManager* QuestManager = CollectedBy->GetQuestManager(ESubGame::GetInvalid()))
    {
        QuestManager->SendItemStatEvent(EFortQuestObjectiveStatEvent::GetCollect(), CollectedItemDefinition, AmountCollected);
    }
#endif
}
