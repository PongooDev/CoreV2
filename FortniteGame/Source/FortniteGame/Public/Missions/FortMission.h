#pragma once

#include "Containers/Array.h"
#include "GameFramework/Actor.h"
#include "Misc/Guid.h"
#include "GameplayTagContainer.h"
#include "Missions/FortObjectiveBase.h"
#include "UObject/Stack.h"

class AFortPlayerController;
class UFortWorldItemDefinition;
class AFortMissionManager;
class UFortMissionInfo;
enum class EFortRewardType : uint8_t;

enum class EFortMissionStatus : uint8_t
{
    Created,
    InProgress,
    Succeeded,
    Failed,
    NeutralCompletion,
    Quit,
    Max_None,
};

class AFortMission : public AActor
{
    DefineUnrealClass(AFortMission, FortniteGame)
    DefineUFunction(StopMissionAIEncounter)
    DefineUFunction(StopEncounterSequence)
    DefineUFunction(GetParticipatingMissionControllers)
    DefineUFunction(GetRewardItemsByTag)
    DefineUFunction(GrantRewardsByTag)

public:
    DefineUProperty(UFortMissionInfo*, MissionInfo)
    DefineUProperty(TArray<AFortObjectiveBase*>, ActiveObjectives)
    DefineUProperty(EFortMissionStatus, MissionStatus)
    DefineUProperty(FGuid, MissionGuid)
    DefineUProperty(FGameplayTagContainer, MissionCompletionTags)

    bool IsPrimaryMission() const;
    static ptrdiff_t MissionInfoOffset();
    TArray<AFortPlayerController*> GetParticipatingMissionControllers();

    static ptrdiff_t ActiveObjectivesOffset();
    void ObjectiveStatusUpdated(AFortObjectiveBase* Objective, EFortObjectiveStatus NewStatus);

    AFortMissionManager* GetMissionManager() const;
    TArray<UFortWorldItemDefinition*> GetRewardItemsByTag(const FGameplayTag& RewardTag);
    void GrantRewardsByTag(const FGameplayTag& RewardTag, EFortRewardType RewardType);

    DECLARE_FUNCTION(execGrantRewardsByTag);

    static void HookCallSites();
    static void HookNatives();
};
