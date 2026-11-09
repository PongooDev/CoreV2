#include "pch.h"
#include "Missions/FortMissionForceSuccessParams.h"
#include "ReflectionParams.h"
#include "UObject/Class.h"

UFortMissionForceSuccessParams* UFortMissionForceSuccessParams::SetParams(float FractionCompleted)
{
    Reflection::FParams Params(StaticFunction_SetParams());
    Params.Set(L"_FractionCompleted", FractionCompleted);
    return Params.Call(this) ? Params.Take<UFortMissionForceSuccessParams*>(L"ThisParam") : nullptr;
}
