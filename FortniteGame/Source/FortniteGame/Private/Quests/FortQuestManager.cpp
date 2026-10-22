#include "pch.h"
#include "Quests/FortQuestManager.h"
#include "Containers/ScriptArray.h"
#include "Finder.h"
#include "Natives.h"
#include "ReflectionParams.h"
#include "UObject/Class.h"

void UFortQuestManager::GetSourceAndContextTags(FGameplayTagContainer& OutSourceTags, FGameplayTagContainer& OutContextTags)
{
    Reflection::FParams Params(StaticFunction_GetSourceAndContextTags(), true);
    if (Params.Call(this))
    {
        OutSourceTags = Params.Take<FGameplayTagContainer>(L"OutSourceTags");
        OutContextTags = Params.Take<FGameplayTagContainer>(L"OutContextTags");
    }
}

UFortQuestItem* UFortQuestManager::GetQuestWithDefinition(UFortQuestItemDefinition* Definition)
{
    Reflection::FParams Params(StaticFunction_GetQuestWithDefinition());
    Params.Set(L"Definition", Definition);
    return Params.Call(this) ? Params.Take<UFortQuestItem*>(L"ReturnValue") : nullptr;
}

bool UFortQuestManager::HasActiveQuest(UFortQuestItemDefinition* Definition)
{
    UFortQuestItem* Quest = Definition ? GetQuestWithDefinition(Definition) : nullptr;
    return Quest && Quest->quest_state == EFortQuestState::GetActive();
}

void UFortQuestManager::SendStatEvent(EFortQuestObjectiveStatEvent Type, UObject* TargetObject, const FGameplayTagContainer& TargetTags, const FGameplayTagContainer& SourceTags, const FGameplayTagContainer& ContextTags, int32_t Count, bool bFlush)
{
    if (Natives::UFortQuestManager_SendStatEvent)
    {
        Natives::UFortQuestManager_SendStatEvent(this, static_cast<uint8_t>(Type), TargetObject, TargetTags, SourceTags, ContextTags, Count, bFlush);
    }
}

void UFortQuestManager::SendEndPrimaryMission(const FString& MatchmakingSessionId, const FString& MissionName, const FString& MissionStatus, int32_t NumMissionPoints, bool bCriticalMatchBonus)
{
    if (Natives::UFortQuestManager_SendEndPrimaryMission)
    {
        FScriptArray EarnedBadges;
        Natives::UFortQuestManager_SendEndPrimaryMission(this, MatchmakingSessionId, MissionName, MissionStatus, EarnedBadges, NumMissionPoints, bCriticalMatchBonus);
    }
}
