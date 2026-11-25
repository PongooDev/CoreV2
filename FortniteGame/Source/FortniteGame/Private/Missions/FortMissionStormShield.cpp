#include "pch.h"
#include "Missions/FortMissionStormShield.h"
#include "Finder.h"
#include "Kismet/GameplayStatics.h"
#include "Log.h"
#include "Logging/LogMacros.h"
#include "Math/Transform.h"
#include "Math/Vector.h"
#include "Missions/FortMissionStormSafeZone.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

DEFINE_LOG_CATEGORY_STATIC(LogFortStormShield, Log, All);

AFortMissionStormSafeZone* AFortMissionStormShield::SpawnSafeAreaAtLocation(const FVector& InLocation, const float InRadius)
{
#if WITH_SERVER_CODE
    UClass* Template = SafeAreaTemplate;
    if (Role != ROLE_Authority || !Template || !Template->IsChildOf(AFortMissionStormSafeZone::StaticClass()))
    {
        return nullptr;
    }
    FTransform SpawnTransform;
    SpawnTransform.SetLocation(InLocation);
    AFortMissionStormSafeZone* SafeArea = Cast<AFortMissionStormSafeZone>(UGameplayStatics::BeginDeferredActorSpawnFromClass(this, Template, SpawnTransform, ESpawnActorCollisionHandlingMethod::AlwaysSpawn, this));
    if (!SafeArea)
    {
        UE_LOG(LogFortStormShield, Error, TEXT("SpawnSafeAreaAtLocation: %s couldn't spawn its %s"), *GetName(), *Template->GetName());
        return nullptr;
    }
    UGameplayStatics::FinishSpawningActor(SafeArea, SpawnTransform);
    SafeArea->SetSafeZoneRadius(InRadius);
    FlushNetDormancy();
    TArray<AFortMissionStormSafeZone*>& Areas = SafeAreas;
    Areas.Add(SafeArea);
    UE_LOG(LogFortStormShield, Log, TEXT("SpawnSafeAreaAtLocation: %s spawned %s at (%.0f, %.0f, %.0f), radius %.0f (%d safe area(s))"), *GetName(), *SafeArea->GetName(),
        InLocation.X, InLocation.Y, InLocation.Z, InRadius, Areas.Num());
    return SafeArea;
#else
    return nullptr;
#endif
}

bool AFortMissionStormShield::RemoveAndDestroySafeArea(AFortMissionStormSafeZone* AreaToRemove)
{
#if WITH_SERVER_CODE
    TArray<AFortMissionStormSafeZone*>& Areas = SafeAreas;
    if (Role != ROLE_Authority || !AreaToRemove || !Areas.Contains(AreaToRemove))
    {
        return false;
    }
    FlushNetDormancy();
    for (int32_t Index = Areas.Num() - 1; Index >= 0; --Index)
    {
        if (Areas[Index] == AreaToRemove)
        {
            Areas.RemoveAt(Index);
        }
    }
    AreaToRemove->K2_DestroyActor();
    UE_LOG(LogFortStormShield, Log, TEXT("RemoveAndDestroySafeArea: %s removed %s (%d safe area(s) left)"), *GetName(), *AreaToRemove->GetName(), Areas.Num());
    return true;
#else
    return false;
#endif
}

DEFINE_FUNCTION(AFortMissionStormShield::execSpawnSafeAreaAtLocation)
{
    P_GET_STRUCT(FVector, Z_Param_InLocation);
    P_GET_PROPERTY(UFloatProperty, Z_Param_InRadius);
    P_FINISH;
    P_NATIVE_BEGIN;
    *static_cast<AFortMissionStormSafeZone**>(RESULT_PARAM) = static_cast<AFortMissionStormShield*>(Context)->SpawnSafeAreaAtLocation(Z_Param_InLocation, Z_Param_InRadius);
    P_NATIVE_END;
}

DEFINE_FUNCTION(AFortMissionStormShield::execRemoveAndDestroySafeArea)
{
    P_GET_OBJECT(AFortMissionStormSafeZone, Z_Param_AreaToRemove);
    P_FINISH;
    P_NATIVE_BEGIN;
    *static_cast<bool*>(RESULT_PARAM) = static_cast<AFortMissionStormShield*>(Context)->RemoveAndDestroySafeArea(Z_Param_AreaToRemove);
    P_NATIVE_END;
}

void AFortMissionStormShield::HookNatives()
{
    // 9.00 (CL-6337466): SpawnSafeAreaAtLocation's exec (0x1B6CAE0) steps the location and radius and returns the null
    // of the shared return-zero stub (UserMathErrorFunction, 0x32B4F0); RemoveAndDestroySafeArea's (0x1B6C270) returns
    // the false of a return-zero stub (0x32D500). Wargames' "Safety Zone" generators (ShieldGeneratorActor_C) got no bubble.
    if (!Member_SafeAreaTemplate.IsAvailable() || !Member_SafeAreas.IsAvailable() || !AFortMissionStormSafeZone::HasSafeZoneRadius())
    {
        return;
    }
    auto Restore = [](UFunction* Function, FNativeFuncPtr Exec, const wchar_t* Name)
    {
        if (!Function)
        {
            return;
        }
        uint8_t* NativeCall = Finder::ImplementationCall(Function);
        if (!NativeCall || !Finder::IsStubFunction(Finder::RelativeTarget(reinterpret_cast<uintptr_t>(NativeCall), 1, 5)))
        {
            Log::Info(L"Finder: AFortMissionStormShield::{} is native in this build", Name);
            return;
        }
        Function->SetNativeFunc(Exec);
        Log::Info(L"Hook AFortMissionStormShield::{}: native", Name);
    };
    Restore(FindStaticFunction_SpawnSafeAreaAtLocation(), &execSpawnSafeAreaAtLocation, L"SpawnSafeAreaAtLocation");
    Restore(FindStaticFunction_RemoveAndDestroySafeArea(), &execRemoveAndDestroySafeArea, L"RemoveAndDestroySafeArea");
}
