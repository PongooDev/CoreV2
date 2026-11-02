#include "pch.h"
#include "FortMissionTimerComponent.h"
#include "ReflectionParams.h"

void UFortMissionTimerComponent::PauseMissionTimer()
{
    Reflection::FParams Params(StaticFunction_PauseMissionTimer());
    Params.Call(this);
}

void UFortMissionTimerComponent::UnpauseMissionTimer()
{
    Reflection::FParams Params(StaticFunction_UnpauseMissionTimer());
    Params.Call(this);
}

bool UFortMissionTimerComponent::IsMissionTimerSet() const
{
    Reflection::FParams Params(StaticFunction_IsMissionTimerSet());
    return Params.Call(const_cast<UFortMissionTimerComponent*>(this)) && Params.Take<bool>(L"ReturnValue");
}

bool UFortMissionTimerComponent::IsMissionTimerPaused() const
{
    Reflection::FParams Params(StaticFunction_IsMissionTimerPaused());
    return Params.Call(const_cast<UFortMissionTimerComponent*>(this)) && Params.Take<bool>(L"ReturnValue");
}

float UFortMissionTimerComponent::GetMissionTimerTimeRemaining() const
{
    Reflection::FParams Params(StaticFunction_GetMissionTimerTimeRemaining());
    return Params.Call(const_cast<UFortMissionTimerComponent*>(this)) ? Params.Take<float>(L"ReturnValue") : 0.0f;
}
