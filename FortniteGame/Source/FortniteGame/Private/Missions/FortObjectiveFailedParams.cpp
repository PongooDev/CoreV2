#include "pch.h"
#include "Missions/FortObjectiveFailedParams.h"
#include "ReflectionParams.h"
#include "UObject/Class.h"

UFortObjectiveFailedParams* UFortObjectiveFailedParams::SetParams(AFortObjectiveBase* FailedObjective)
{
    Reflection::FParams Params(StaticFunction_SetParams());
    Params.Set(L"_FailedObjective", FailedObjective);
    return Params.Call(this) ? Params.Take<UFortObjectiveFailedParams*>(L"ThisParam") : nullptr;
}
