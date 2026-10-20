#include "pch.h"
#include "Missions/FortMission.h"
#include "Finder.h"
#include "Natives.h"
#include "ObjectLayout.h"

ptrdiff_t AFortMission::ActiveObjectivesOffset()
{
    return Member_ActiveObjectives.GetOffsetChecked();
}

void AFortMission::ObjectiveStatusUpdated(AFortObjectiveBase* Objective, EFortObjectiveStatus NewStatus)
{
    if (Natives::AFortMission_ObjectiveStatusUpdatedIndex < 0 && !Finder::ResolveMissionObjectivesLive())
    {
        return;
    }
    CallVirtual<void>(this, Natives::AFortMission_ObjectiveStatusUpdatedIndex, Objective, NewStatus);
}
