#pragma once

#include "Containers/UnrealString.h"
#include "GameplayTagContainer.h"
#include "UObject/Object.h"

class EFortQuestObjectiveStatEvent
{
    DefineUnrealEnum(EFortQuestObjectiveStatEvent, FortniteGame)
    DefineUEnumValue(Kill)
    DefineUEnumValue(KillContribution)
    DefineUEnumValue(Build)
    DefineUEnumValue(BuildingEdit)
    DefineUEnumValue(BuildingRepair)
    DefineUEnumValue(BuildingUpgrade)
    DefineUEnumValue(Complete)
    DefineUEnumValue(Craft)
    DefineUEnumValue(Collect)
    DefineUEnumValue(Win)
    DefineUEnumValue(Interact)
    DefineUEnumValue(Destroy)
    DefineUEnumValue(Ability)
    DefineUEnumValue(WaveComplete)
    DefineUEnumValue(Custom)
    DefineUEnumValue(Client)
    DefineUEnumValue(NumGameplayEvents)
    DefineUEnumValue(Acquire)
    DefineUEnumValue(Consume)
    DefineUEnumValue(OpenCardPack)
    DefineUEnumValue(PurchaseCardPack)
    DefineUEnumValue(Convert)
    DefineUEnumValue(Upgrade)
    DefineUEnumValue(QuestComplete)
    DefineUEnumValue(AssignWorker)
    DefineUEnumValue(CollectExpedition)
    DefineUEnumValue(CollectSuccessfulExpedition)
    DefineUEnumValue(HasItem)
    DefineUEnumValue(SlotInCollection)
    DefineUEnumValue(HasCompletedQuest)
    DefineUEnumValue(HasAssignedWorker)
    DefineUEnumValue(HasUpgraded)
    DefineUEnumValue(HasConverted)
    DefineUEnumValue(Max_None)
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
