#pragma once

#include "Athena/AthenaBarrierObjective.h"
#include "Building/BuildingActor.h"

// A team's restaurant in Food Fight (from 6.30; TeamObjective_Food1_C, the Barrier mutator's ObjectiveFlag): the
// under-construction building (its AthenaBuildingActorChildComponents, TeamObjective_Floor and _Roof) with the team's
// mascot (a ChildActorComponent with TeamGenerator_C, an AAthenaBarrierObjective) on it. An ABuildingGameplayActor (Core
// declares none, so this derives from its ABuildingActor). Its FoodTeam picks the burger or the tomato head.
class AAthenaBarrierFlag : public ABuildingActor
{
    DefineUnrealClass(AAthenaBarrierFlag, FortniteGame)
    DefineUFunction(GetObjectiveActor)

public:
    DefineUProperty(EBarrierFoodTeam, FoodTeam)

    // The restaurant's mascot: its blueprint's GetObjectiveActor (the ChildActorComponent's ChildActor).
    AAthenaBarrierObjective* GetObjectiveActor() const;
};
