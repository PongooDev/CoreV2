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
    if (UFortQuestManager* QuestManager = CollectedBy->GetQuestManager())
    {
        QuestManager->SendItemStatEvent(EFortQuestObjectiveStatEvent::GetCollect(), CollectedItemDefinition, AmountCollected);
    }
#endif
}
