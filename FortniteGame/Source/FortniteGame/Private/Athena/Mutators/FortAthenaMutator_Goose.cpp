#include "pch.h"
#include "Athena/Mutators/FortAthenaMutator_Goose.h"
#include "Athena/AthenaGoosePickup.h"
#include "Athena/FortAthenaVehicle.h"
#include "Athena/FortPlayerPawnAthena.h"
#include "Athena/FortPlayerStateAthena.h"
#include "Engine/World.h"
#include "Finder.h"
#include "Hooks.h"
#include "Log.h"
#include "Logging/LogMacros.h"
#include "Math/Transform.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

DEFINE_LOG_CATEGORY_STATIC(LogGooseMutator, Log, All);

AFortAthenaVehicle* AFortAthenaMutator_Goose::SpawnPlane()
{
#if WITH_SERVER_CODE
    UClass* PlaneClass = VehicleClass;
    UWorld* World = PlaneClass ? GetWorld() : nullptr;
    if (!World || Role != ROLE_Authority)
    {
        return nullptr;
    }

    // As the build's remnant does: from PlaneSpawnLocation, the sleeping plane furthest along X; the new one is parked
    // past it (Core's spacing).
    FVector SpawnLocation = PlaneSpawnLocation;
    bool bAnySleeping = false;
    TArray<AFortAthenaVehicle*>& Vehicles = SpawnedVehicles;
    for (int32_t Index = 0; Index < Vehicles.Num(); ++Index)
    {
        AFortAthenaVehicle* Vehicle = Vehicles[Index];
        if (Vehicle && !Vehicle->IsPendingKill() && Vehicle->IsAsleep())
        {
            const FVector Location = Vehicle->GetTransform().GetLocation();
            if (Location.X > SpawnLocation.X)
            {
                SpawnLocation = Location;
            }
            bAnySleeping = true;
        }
    }
    if (bAnySleeping)
    {
        SpawnLocation.X += SleepingPlaneSpacing;
    }

    FTransform SpawnTransform;
    SpawnTransform.SetLocation(SpawnLocation);
    FActorSpawnParameters SpawnInfo;
    SpawnInfo.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    AFortAthenaVehicle* Plane = Cast<AFortAthenaVehicle>(World->SpawnActor(PlaneClass, &SpawnTransform, SpawnInfo));
    if (!Plane)
    {
        UE_LOG(LogGooseMutator, Error, TEXT("SpawnPlane: %s couldn't spawn a %s"), *GetName(), *PlaneClass->GetName());
        return nullptr;
    }

    // OnVehicleDied unbinds both (RemoveAll on each) when the plane dies.
    FMulticastScriptDelegate& OnOutOfHealth = Plane->OnHandleOutOfHealthDelegate;
    OnOutOfHealth.AddUniqueDynamic(this, StaticFunction_OnVehicleDied()->GetFName());
    FMulticastScriptDelegate& OnSquadChanged = Plane->OnVehicleSquadChanged;
    OnSquadChanged.AddUniqueDynamic(this, StaticFunction_OnVehicleStolen()->GetFName());
    Vehicles.Add(Plane);
    UE_LOG(LogGooseMutator, Log, TEXT("SpawnPlane: %s parked %s at (%.0f, %.0f, %.0f), %d planes"), *GetName(), *Plane->GetName(), SpawnLocation.X, SpawnLocation.Y, SpawnLocation.Z,
        Vehicles.Num());
    return Plane;
#else
    return nullptr;
#endif
}

AFortAthenaVehicle* AFortAthenaMutator_Goose::SpawnPlaneCall(AFortAthenaMutator_Goose* This)
{
    return This ? This->SpawnPlane() : nullptr;
}

void AFortAthenaMutator_Goose::OnPawnDied(AFortPlayerPawnAthena* PlayerPawn)
{
#if WITH_SERVER_CODE
    if (!PlayerPawn)
    {
        UE_LOG(LogGooseMutator, Warning, TEXT("[AFortAthenaMutator_Goose::OnPawnDied()] Player Pawn is Null! Not sure what to do!"));
        return;
    }
    if (Role != ROLE_Authority)
    {
        return;
    }
    // Once per pawn: SeatPlayer adds the binding without checking for one.
    FMulticastScriptDelegate& OnFortPlayerDied = PlayerPawn->OnFortPlayerDied;
    OnFortPlayerDied.RemoveDynamic(this, StaticFunction_OnPawnDied()->GetFName());

    AFortPlayerStateAthena* PlayerState = Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(static_cast<APlayerState*>(PlayerPawn->PlayerState)));
    const uint8_t SquadId = PlayerState ? static_cast<uint8_t>(PlayerState->SquadId) : 0;
    if (!SquadId)
    {
        return;
    }
    FPlaneSquadInfoArray& Squads = SquadInfoArray;
    TStructArray<FPlaneSquadInfo>& SquadData = Squads.SquadData;
    for (int32_t Index = 0; Index < SquadData.Num(); ++Index)
    {
        FPlaneSquadInfo& Squad = SquadData[Index];
        if (static_cast<uint8_t>(Squad.SquadId) != SquadId)
        {
            continue;
        }
        if (Squad.PlanesRemaining > 0)
        {
            Squad.PlanesRemaining = Squad.PlanesRemaining - 1;
            Squads.MarkItemDirty(Squad);
            ForceNetUpdate();
        }
        UE_LOG(LogGooseMutator, Log, TEXT("OnPawnDied: %s of squad %d died, %d planes left"), *PlayerPawn->GetName(), static_cast<int32_t>(SquadId), static_cast<int32_t>(Squad.PlanesRemaining));
        return;
    }
#endif
}

DEFINE_FUNCTION(AFortAthenaMutator_Goose::execOnPawnDied)
{
    P_GET_OBJECT(AFortPlayerPawnAthena, Z_Param_PlayerPawn);
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<AFortAthenaMutator_Goose*>(Context)->OnPawnDied(Z_Param_PlayerPawn);
    P_NATIVE_END;
}

void AFortAthenaMutator_Goose::HookOnPawnDied()
{
#if WITH_SERVER_CODE
    // 8.40's native (0xFE9AB0) is only the null check's log: one call (the log) and nothing after it.
    UFunction* Function = FindStaticFunction_OnPawnDied();
    uint8_t* NativeCall = Function ? Finder::ImplementationCall(Function) : nullptr;
    const uintptr_t Native = NativeCall ? Finder::RelativeTarget(reinterpret_cast<uintptr_t>(NativeCall), 1, 5) : 0;
    const bool bStripped = Native && Finder::FunctionStart(Native) == Native && Finder::FunctionEnd(Native) - Native < 0x50 && Finder::DecodedDirectCalls(Native).size() == 1;
    if (!bStripped || !FPlaneSquadInfo::StaticStruct() || !FPlaneSquadInfoArray::StaticStruct() || !Member_SquadInfoArray.IsAvailable() || !AFortPlayerPawnAthena::HasOnFortPlayerDied()
        || !AFortPlayerStateAthena::HasSquadId())
    {
        Log::Info(L"Air Royale: OnPawnDied is not restored (native 0x{:X}, stripped {})", Native ? Native - Finder::ImageBase() : 0, bStripped);
        return;
    }
    Function->SetNativeFunc(&execOnPawnDied);
    Log::Info(L"Air Royale: OnPawnDied is stripped (0x{:X}), restored: a seated player's death costs their squad a plane", Native - Finder::ImageBase());
#endif
}

void AFortAthenaMutator_Goose::HookNatives()
{
#if WITH_SERVER_CODE
    if (!StaticClass())
    {
        return;
    }
    Finder::FGooseMutatorSites Sites;
    if (!HasPlaneSpawning() || !AFortAthenaVehicle::HasPlaneDelegates() || !Finder::ResolveGooseMutatorLive(Sites))
    {
        Log::Info(L"Air Royale: the mutator's planes are not restored (members {}, vehicle delegates {})", HasPlaneSpawning(), AFortAthenaVehicle::HasPlaneDelegates());
        return;
    }
    HookOnPawnDied();
    for (int32_t Index = 0; Index < Sites.NumSpawnPlaneCalls; ++Index)
    {
        Hooks::RedirectCall(L"AFortAthenaMutator_Goose::SpawnPlane", Sites.SpawnPlaneCalls[Index], reinterpret_cast<void*>(&SpawnPlaneCall));
    }
    const bool bPickups = Sites.SpawnPickupCall && AAthenaGoosePickup::LootTableNameOffset() >= 0 && AAthenaGoosePickup::DeployAltitudeOffset() >= 0;
    if (bPickups)
    {
        Hooks::RedirectCall(L"AFortAthenaMutator_Goose pickup timer -> AAthenaGoosePickup::SpawnGoosePickup", Sites.SpawnPickupCall, reinterpret_cast<void*>(&AAthenaGoosePickup::SpawnGoosePickup));
    }
    Log::Info(L"Air Royale: SpawnPlane is stripped, restored at {} calls; the floating upgrades' spawn {} (loot table +0x{:X}, height +0x{:X})", Sites.NumSpawnPlaneCalls,
        bPickups ? L"restored" : L"not restored", AAthenaGoosePickup::LootTableNameOffset(), AAthenaGoosePickup::DeployAltitudeOffset());
#endif
}
