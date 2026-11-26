#pragma once

#include "Containers/Array.h"
#include "GameFramework/Actor.h"
#include "Misc/Guid.h"
#include "GameplayTagContainer.h"
#include "Missions/FortObjectiveBase.h"
#include "UObject/Stack.h"

class AFortPlayerController;
class UFortBadgeItemDefinition;
class UFortWorldItemDefinition;
class AFortMissionManager;
class UFortMissionInfo;
class UFortMissionTimerComponent;
class EFortRewardType;

class EFortMissionStatus
{
    DefineUnrealEnum(EFortMissionStatus, FortniteGame)
    DefineUEnumValue(Created)
    DefineUEnumValue(InProgress)
    DefineUEnumValue(Succeeded)
    DefineUEnumValue(Failed)
    DefineUEnumValue(NeutralCompletion)
    DefineUEnumValue(Quit)
    DefineUEnumValue(Max_None)
};

struct FFortMissionEvent
{
    DefineUnrealStruct(FFortMissionEvent, FortniteGame)
    DefineUProperty(FName, EventType)
};

class AFortMission : public AActor
{
    DefineUnrealClass(AFortMission, FortniteGame)
    DefineUFunction(StopMissionAIEncounter)
    DefineUFunction(StopEncounterSequence)
    DefineUFunction(GetParticipatingMissionControllers)
    DefineUFunction(GetRewardItemsByTag)
    DefineUFunction(GrantRewardsByTag)
    DefineUFunction(GetAdditionalMissionSuccessBadges)
    DefineUFunction(GetAdditionalMissionCompletionMissionPoints)
    DefineUFunction(BlueprintCustomHandleMissionEvent)
    DefineUFunction(GetDaysRemainingBeforeOverPar)
    DefineUFunction(InitializeActorsForMission)
    DefineUFunction(SpawnAtPlacementActorsAsArray)
    DefineUFunction(AddOrRemoveTimerTime)

public:
    DefineUProperty(UFortMissionInfo*, MissionInfo)
    DefineBitfieldUProperty(bFiredParTimeEvent)
    DefineUProperty(TArray<AFortObjectiveBase*>, ActiveObjectives)
    DefineUProperty(EFortMissionStatus, MissionStatus)
    DefineUProperty(FGuid, MissionGuid)
    DefineUProperty(FGameplayTagContainer, MissionCompletionTags)
    DefineUProperty(UFortMissionTimerComponent*, TimerComponent)

    static bool HasTimerComponent() { return Member_TimerComponent.IsAvailable(); }

    bool IsPrimaryMission() const;
    static ptrdiff_t MissionInfoOffset();
    TArray<AFortPlayerController*> GetParticipatingMissionControllers();

    static ptrdiff_t ActiveObjectivesOffset();
    void ObjectiveStatusUpdated(AFortObjectiveBase* Objective, EFortObjectiveStatus NewStatus);

    AFortMissionManager* GetMissionManager() const;
    TArray<UFortWorldItemDefinition*> GetRewardItemsByTag(const FGameplayTag& RewardTag);
    void GrantRewardsByTag(FGameplayTag RewardTag, EFortRewardType RewardType);
    void GrantBadge(UFortBadgeItemDefinition* Badge, EFortRewardType RewardType);

    DECLARE_FUNCTION(execGrantRewardsByTag);
    static void GrantRewardsByTagCall(AFortMission* This, FGameplayTag RewardTag, EFortRewardType RewardType);
    static TArray<AActor*>* SpawnAtPlacementActorsAsArrayCall(TArray<AActor*>* Result, AFortMission* Mission, bool* bSucceeded);

    bool GetAdditionalMissionSuccessBadges(TArray<UFortWorldItemDefinition*>& SuccessItems);
    int32_t GetAdditionalMissionCompletionMissionPoints(EFortMissionStatus CompletionStatus);
    void BlueprintCustomHandleMissionEvent(const FFortMissionEvent& MissionEvent);
    void CustomHandleMissionEvent(const FFortMissionEvent& MissionEvent);
    void HandleDeferredMissionEvent(const FFortMissionEvent& MissionEvent);
    int32_t GetDaysRemainingBeforeOverPar();
    bool IsUnderPar();
    // 8.20+: moves the mission's own timer (TimerComponent) by TimeToAdd seconds; Wargames' Time Cheats call it with the
    // wave's WaveTimeToAdd. A BlueprintNativeEvent.
    void AddOrRemoveTimerTime_Implementation(float TimeToAdd);

    DECLARE_FUNCTION(execGetAdditionalMissionSuccessBadges);
    DECLARE_FUNCTION(execGetAdditionalMissionCompletionMissionPoints);
    DECLARE_FUNCTION(execBlueprintCustomHandleMissionEvent);

    static void HookCallSites();
    static void HookNatives();
    static void HookVTables();

private:
    static inline void (*CustomHandleMissionEventOG)(AFortMission* This, const FFortMissionEvent& MissionEvent) = nullptr;
    static void CustomHandleMissionEventHK(AFortMission* This, const FFortMissionEvent& MissionEvent);
    static inline void (*HandleDeferredMissionEventOG)(AFortMission* This, const FFortMissionEvent& MissionEvent) = nullptr;
    static void HandleDeferredMissionEventHK(AFortMission* This, const FFortMissionEvent& MissionEvent);
    static void AddOrRemoveTimerTimeHK(AFortMission* This, float TimeToAdd);
    static void HookAddOrRemoveTimerTime();
};

class AFortAIPawn;
class UFortAIEncounterInfo;

// The Cube Monsters' rift mission (6.20's Fortnitemares, and Creative's creature spawners from 8.00).
class AFortMission_RiftSpawners : public AFortMission
{
    DefineUnrealClass(AFortMission_RiftSpawners, FortniteGame)
    DefineUFunction(HandleAISpawned)

public:
    // Bound to each rift encounter's OnEncounterEnemySpawned (the mission's encounter start, 8.20 0x136AC70).
    void HandleAISpawned(UFortAIEncounterInfo* Encounter, AFortAIPawn* SpawnedEnemy);
    DECLARE_FUNCTION(execHandleAISpawned);

    static void HookNatives();
};
