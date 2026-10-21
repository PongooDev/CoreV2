#pragma once

#include "Containers/Array.h"
#include "GameFramework/Actor.h"
#include "Misc/Guid.h"
#include "Missions/FortObjectiveBase.h"

class AFortPlayerController;
class UFortMissionInfo;

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

public:
    DefineUProperty(UFortMissionInfo*, MissionInfo)
    DefineUProperty(TArray<AFortObjectiveBase*>, ActiveObjectives)
    DefineUProperty(EFortMissionStatus, MissionStatus)
    DefineUProperty(FGuid, MissionGuid)

    bool IsPrimaryMission() const;
    static ptrdiff_t MissionInfoOffset();
    TArray<AFortPlayerController*> GetParticipatingMissionControllers();

    static ptrdiff_t ActiveObjectivesOffset();
    void ObjectiveStatusUpdated(AFortObjectiveBase* Objective, EFortObjectiveStatus NewStatus);

    static void HookCallSites();
};
