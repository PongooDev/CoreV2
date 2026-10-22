#pragma once

#include "Containers/UnrealString.h"
#include "GameplayTagContainer.h"
#include "UObject/Object.h"

enum class EFortQuestObjectiveStatEvent : uint8_t
{
    Complete = 6,
};

class UFortMcpProfileAccount;
class UFortMcpProfileWorld;

class UFortQuestManager : public UObject
{
    DefineUnrealClass(UFortQuestManager, FortniteGame)
    DefineUFunction(GetSourceAndContextTags)

public:
    void GetSourceAndContextTags(FGameplayTagContainer& OutSourceTags, FGameplayTagContainer& OutContextTags);
    void SendStatEvent(EFortQuestObjectiveStatEvent Type, UObject* TargetObject, const FGameplayTagContainer& TargetTags, const FGameplayTagContainer& SourceTags, const FGameplayTagContainer& ContextTags, int32_t Count, bool bFlush);
    void SendEndPrimaryMission(const FString& MatchmakingSessionId, const FString& MissionName, const FString& MissionStatus, int32_t NumMissionPoints, bool bCriticalMatchBonus);
};

class UFortRegisteredPlayerInfo : public UObject
{
    DefineUnrealClass(UFortRegisteredPlayerInfo, FortniteGame)

public:
    DefineUProperty(UFortMcpProfileAccount*, AccountProfile)
    DefineUProperty(UFortMcpProfileWorld*, WorldProfile)
    DefineUProperty(UFortQuestManager*, QuestManager)
};
