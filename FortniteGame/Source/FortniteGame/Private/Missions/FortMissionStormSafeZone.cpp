#include "pch.h"
#include "Missions/FortMissionStormSafeZone.h"
#include "Finder.h"
#include "Log.h"
#include "ReflectionParams.h"
#include "UObject/Class.h"

void AFortMissionStormSafeZone::SetSafeZoneRadius(const float InRadius)
{
#if WITH_SERVER_CODE
    if (Role != ROLE_Authority)
    {
        return;
    }
    FlushNetDormancy();
    SafeZoneRadius = InRadius;
    // The server's own mesh and Blueprint follow the radius as a client's do.
    OnRep_ZoneRadius();
#endif
}

void AFortMissionStormSafeZone::OnRep_ZoneRadius()
{
    Reflection::FParams Params(StaticFunction_OnRep_ZoneRadius());
    Params.Call(this);
}

DEFINE_FUNCTION(AFortMissionStormSafeZone::execSetSafeZoneRadius)
{
    P_GET_PROPERTY(UFloatProperty, Z_Param_InRadius);
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<AFortMissionStormSafeZone*>(Context)->SetSafeZoneRadius(Z_Param_InRadius);
    P_NATIVE_END;
}

void AFortMissionStormSafeZone::HookNatives()
{
    // 9.00 (CL-6337466): SetSafeZoneRadius's exec (0x1A68420, folded with UFortCheatManager::execEncounterSetDifficultyLevel)
    // steps the float and calls an empty function (0x32B4B0). OnRep_ZoneRadius (0x15975D0) is intact.
    UFunction* Function = FindStaticFunction_SetSafeZoneRadius();
    if (!Function || !HasSafeZoneRadius() || !FindStaticFunction_OnRep_ZoneRadius())
    {
        return;
    }
    uint8_t* NativeCall = Finder::ImplementationCall(Function);
    if (!NativeCall || !Finder::IsStubFunction(Finder::RelativeTarget(reinterpret_cast<uintptr_t>(NativeCall), 1, 5)))
    {
        Log::Info(L"Finder: AFortMissionStormSafeZone::SetSafeZoneRadius is native in this build");
        return;
    }
    Function->SetNativeFunc(&execSetSafeZoneRadius);
    Log::Info(L"Hook AFortMissionStormSafeZone::SetSafeZoneRadius: native");
}
