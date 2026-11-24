#include "pch.h"
#include "Athena/Mutators/FortAthenaMutator_Goose.h"
#include "Athena/AthenaGoosePickup.h"
#include "Athena/FortAthenaVehicle.h"
#include "Engine/World.h"
#include "Finder.h"
#include "Hooks.h"
#include "Log.h"
#include "Logging/LogMacros.h"
#include "Math/Transform.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"

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
