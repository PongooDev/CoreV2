#include "pch.h"
#include "Missions/FortMission.h"
#include "AI/FortAIDirector.h"
#include "Finder.h"
#include "Hooks.h"
#include "Log.h"
#include "Natives.h"
#include "ObjectLayout.h"
#include "UObject/Class.h"

void AFortMission::HookCallSites()
{
    for (UFunction* Function : { StaticFunction_StopMissionAIEncounter(), StaticFunction_StopEncounterSequence() })
    {
        if (uint8_t* Call = Finder::NullGetCurrentCall(Function))
        {
            Hooks::RedirectCall(L"AFortAIDirector::GetCurrent", Call, reinterpret_cast<void*>(&AFortAIDirector::GetCurrent));
        }
        else
        {
            Log::Error(L"Finder: AFortAIDirector::GetCurrent call in {} not found", Function ? Function->GetName().ToStdString() : std::wstring(L"(none)"));
        }
    }
}

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
