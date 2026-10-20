#pragma once

#include "Containers/Array.h"
#include "GameFramework/Actor.h"
#include "Missions/FortObjectiveBase.h"

class AFortMission : public AActor
{
    DefineUnrealClass(AFortMission, FortniteGame)
    DefineUFunction(StopMissionAIEncounter)
    DefineUFunction(StopEncounterSequence)

public:
    DefineUProperty(TArray<AFortObjectiveBase*>, ActiveObjectives)

    static ptrdiff_t ActiveObjectivesOffset();
    void ObjectiveStatusUpdated(AFortObjectiveBase* Objective, EFortObjectiveStatus NewStatus);

    static void HookCallSites();
};
