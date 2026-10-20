#pragma once

#include "GameFramework/Actor.h"
#include "Missions/FortObjectiveBase.h"

class AFortMission : public AActor
{
    DefineUnrealClass(AFortMission, FortniteGame)

public:
    void ObjectiveStatusUpdated(AFortObjectiveBase* Objective, EFortObjectiveStatus NewStatus);
};
