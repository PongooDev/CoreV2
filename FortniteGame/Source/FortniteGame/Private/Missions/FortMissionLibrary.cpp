#include "pch.h"
#include "Missions/FortMissionLibrary.h"
#include "ReflectionParams.h"
#include "UObject/Class.h"

void UFortMissionLibrary::ExecuteMissionEvent(UObject* WorldContextObject, FName EventType, UObject* Params)
{
    Reflection::FParams Parms(StaticFunction_ExecuteMissionEvent(), true);
    Parms.Set(L"WorldContextObject", WorldContextObject);
    Parms.Set(L"EventType", EventType);
    Parms.Set(L"Params", Params);
    Parms.Call(StaticClass()->GetDefaultObject());
}

void UFortMissionLibrary::ExecuteMissionEvent(UObject* WorldContextObject, FName EventType, const FGuid& MissionGuid, const FGameplayTagContainer& ObjectiveHandle, UObject* Params)
{
    Reflection::FParams Parms(StaticFunction_ExecuteMissionEvent(), true);
    Parms.Set(L"WorldContextObject", WorldContextObject);
    Parms.Set(L"EventType", EventType);
    Parms.Set(L"MissionGuid", MissionGuid);
    Parms.Set(L"ObjectiveHandle", ObjectiveHandle);
    Parms.Set(L"Params", Params);
    Parms.Call(StaticClass()->GetDefaultObject());
}
