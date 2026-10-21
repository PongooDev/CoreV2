#include "pch.h"
#include "Missions/FortObjectiveSucceededParams.h"
#include "ReflectionParams.h"
#include "UObject/Class.h"

UFortObjectiveSucceededParams* UFortObjectiveSucceededParams::SetParams(AFortObjectiveBase* SucceededObjective)
{
    Reflection::FParams Params(StaticFunction_SetParams());
    Params.Set(L"_SucceededObjective", SucceededObjective);
    return Params.Call(this) ? Params.Take<UFortObjectiveSucceededParams*>(L"ThisParam") : nullptr;
}
