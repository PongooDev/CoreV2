#pragma once

#include "Building/BuildingActor.h"

// A low-gravity zone (from 6.00, an ABuildingGameplayActor). The cube's vortex under the floating island
// (BGA_DudeBro_Area_Effect_C) is one: its funnel, Sm_DudeBro_VortexBase_01, is a cone round the actor's vertical axis,
// 10 m in radius at its foot and 74 m at its top, 91 m up, and a pawn that enters it is in the vortex
// (AFortPlayerPawn::bIsInVortex) until it leaves it.
class AFortAthenaLowGravZone : public ABuildingActor
{
    DefineUnrealClass(AFortAthenaLowGravZone, FortniteGame)
};
