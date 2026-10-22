#include "pch.h"
#include "AI/FortNavSystem.h"
#include "ReflectionParams.h"

void UFortNavSystem::OnNavDataRegistered(ANavigationData* NavData)
{
    Reflection::FParams Params(StaticFunction_OnNavDataRegistered());
    Params.Set(L"NavData", reinterpret_cast<UObject*>(NavData));
    Params.Call(this);
}
