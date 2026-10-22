#include "pch.h"
#include "Missions/FortObjectiveNeutralCompleteParams.h"
#include "ReflectionParams.h"
#include "UObject/Class.h"

UFortObjectiveNeutralCompleteParams* UFortObjectiveNeutralCompleteParams::SetParams(AFortObjectiveBase* NeutrallyCompletedObjective)
{
    Reflection::FParams Params(StaticFunction_SetParams());
    Params.Set(L"_NeutrallyCompletedObjective", NeutrallyCompletedObjective);
    return Params.Call(this) ? Params.Take<UFortObjectiveNeutralCompleteParams*>(L"ThisParam") : nullptr;
}
