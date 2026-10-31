#pragma once

#include "Containers/Map.h"
#include "Containers/UnrealString.h"
#include "GameFramework/OnlineReplStructs.h"
#include "GameplayTagContainer.h"
#include "StructArray.h"
#include "UObject/Object.h"
#include "UObject/WeakObjectPtr.h"

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

class EFortQuestState
{
    DefineUnrealEnum(EFortQuestState, FortniteGame)
    DefineUEnumValue(Inactive)
    DefineUEnumValue(Active)
    DefineUEnumValue(Completed)
    DefineUEnumValue(Claimed)
};

struct FFortQuestEarnedBadgeData
{
    DefineUnrealStruct(FFortQuestEarnedBadgeData, FortniteGame)
    DefineUProperty(FString, TemplateId)
    DefineUProperty(int32_t, Count)
};

class AFortPlayerController;
class UFortHero;
class UFortItemDefinition;
class UFortMcpProfileAccount;
class UFortMcpProfileWorld;
class UFortQuestItemDefinition;

// What a quest manager's flush sends its profile as UpdateQuests' advance: an objective's BackendName and how far the
// stat events since the last flush moved it (a delta; the backend adds it to the objective's count).
struct FFortQuestObjectiveCompletion
{
    DefineUnrealStruct(FFortQuestObjectiveCompletion, FortniteGame)
    DefineUProperty(FString, StatName)
    DefineUProperty(int32_t, Count)
};

// One of a quest's objectives, as the quest item has it from its profile (the counts are the profile's).
class UFortQuestObjectiveInfo : public UObject
{
    DefineUnrealClass(UFortQuestObjectiveInfo, FortniteGame)

public:
    DefineUProperty(FName, BackendName)
    DefineUProperty(int32_t, AchievedCount)
    DefineUProperty(int32_t, RequiredCount)
};

class UFortQuestItem : public UObject
{
    DefineUnrealClass(UFortQuestItem, FortniteGame)

public:
    DefineUProperty(EFortQuestState, quest_state)
    DefineUProperty(TArray<UFortQuestObjectiveInfo*>, Objectives)

    // The quest's UFortQuestItemDefinition (UFortItem::GetItemDefinitionBP).
    UFortItemDefinition* GetQuestDefinition() const;
    static bool HasObjectives() { return Member_Objectives.IsAvailable(); }
};

class UFortQuestManager : public UObject
{
    DefineUnrealClass(UFortQuestManager, FortniteGame)
    DefineUFunction(GetSourceAndContextTags)
    DefineUFunction(GetQuestWithDefinition)

public:
    // The quests the manager's profile holds, as quest items (the server's copy for a player).
    DefineUProperty(TArray<UFortQuestItem*>, CurrentQuests)
    // The progress stat events made since the last flush, which the flush sends as UpdateQuests.
    DefineUStructArrayProperty(FFortQuestObjectiveCompletion, PendingChanges)
    // The account profile the progress goes to.
    DefineUProperty(FWeakObjectPtr, BoundProfile)

    void GetSourceAndContextTags(FGameplayTagContainer& OutSourceTags, FGameplayTagContainer& OutContextTags);
    UFortQuestItem* GetQuestWithDefinition(UFortQuestItemDefinition* Definition);
    bool HasActiveQuest(UFortQuestItemDefinition* Definition);
    static bool HasCurrentQuests() { return Member_CurrentQuests.IsAvailable() && Member_PendingChanges.IsAvailable(); }
    // Server ("cheat progressquest", #155): the objective of Quest moves Count on the way SendStatEventWithTags moves it
    // (1.9 0x8A5FB0): Count joins the manager's pending progress for the objective's BackendName, the flush sends it
    // to the profile as UpdateQuests at once, and the player's UpdatedObjectiveStats (replicated) shows the client the
    // objective's new count. Returns the objective's count afterwards, or -1 when this build can't send it.
    int32_t AdvanceObjective(AFortPlayerController* PlayerController, UFortQuestItem* Quest, UFortQuestObjectiveInfo* Objective, int32_t Count);
    void SendStatEvent(EFortQuestObjectiveStatEvent Type, UObject* TargetObject, const FGameplayTagContainer& TargetTags, const FGameplayTagContainer& SourceTags, const FGameplayTagContainer& ContextTags, int32_t Count, bool bFlush);
    void SendItemStatEvent(EFortQuestObjectiveStatEvent Type, UFortItemDefinition* ItemDefinition, int32_t Count);
    void SendEndPrimaryMission(const FString& MatchmakingSessionId, const FString& MissionName, const FString& MissionStatus, TStructArray<FFortQuestEarnedBadgeData>& EarnedBadges, int32_t NumMissionPoints, bool bCriticalMatchBonus);

    // Saves Battle Royale's challenge progress (1.8), which the game keeps on the server.
    static void Hook();

private:
    static void SendStatEventWithTagsHK(UFortQuestManager* This, const void* CustomStatName, uint8_t Type, UObject* TargetObject, const FGameplayTagContainer& TargetTags, const FGameplayTagContainer& SourceTags, const FGameplayTagContainer& ContextTags, int32_t Count, bool bFlush);
};

class UFortRegisteredPlayerInfo : public UObject
{
    using FQuestManagers = TMap<uint8_t, UFortQuestManager*>;

    DefineUnrealClass(UFortRegisteredPlayerInfo, FortniteGame)

public:
    DefineUProperty(FUniqueNetIdRepl, PlayerID)
    DefineUProperty(UFortMcpProfileAccount*, AccountProfile)
    DefineUProperty(UFortMcpProfileWorld*, WorldProfile)
    DefineUProperty(UFortQuestManager*, QuestManager)
    DefineUProperty(UFortHero*, AthenaMenuHeroDef)

private:
    // 1.8: a quest manager per sub game (ESubGame: Campaign 0, Athena 1) instead of QuestManager. A TMap can't be copied,
    // so it has no DefineUProperty accessors.
    static inline Reflection::FMemberInfo Member_QuestManagers{ &StaticReflectionOwner, TEXT("QuestManagers"), sizeof(FQuestManagers), Reflection::EMemberKind::Value };

public:
    // Save the World's quest manager (1.8 keeps Battle Royale's apart).
    UFortQuestManager* GetCampaignQuestManager()
    {
        constexpr uint8_t Campaign = 0;
        return Member_QuestManager.IsAvailable() ? static_cast<UFortQuestManager*>(QuestManager)
            : Member_QuestManagers.IsAvailable() ? Reflection::AccessMember<FQuestManagers>(this, Member_QuestManagers).FindRef(Campaign) : nullptr;
    }

    // Battle Royale's quest manager (1.8: the athena profile's challenges), null before 1.8.
    UFortQuestManager* GetAthenaQuestManager()
    {
        constexpr uint8_t Athena = 1;
        return Member_QuestManagers.IsAvailable() ? Reflection::AccessMember<FQuestManagers>(this, Member_QuestManagers).FindRef(Athena) : nullptr;
    }
};
