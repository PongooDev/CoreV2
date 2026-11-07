#include "pch.h"
#include "AI/Navigation/NavigationSystem.h"
#include "Natives.h"
#include "ReflectionParams.h"

void UNavigationSystem::RemoveNavigationBuildLock(uint8_t Flags)
{
    const bool bWasLocked = IsNavigationBuildingLocked();

    NavBuildingLockFlags = static_cast<uint8_t>(NavBuildingLockFlags & ~Flags);

    const bool bIsLocked = IsNavigationBuildingLocked();
    if (bWasLocked && !bIsLocked)
    {
        RebuildAll();
    }
}

void UNavigationSystem::ReleaseInitialBuildingLock()
{
    RemoveNavigationBuildLock(ENavigationBuildLock::InitialLock);
}

void UNavigationSystem::RebuildAll(bool bIsLoadTime)
{
    if (Natives::UNavigationSystem_RebuildAll)
    {
        Natives::UNavigationSystem_RebuildAll(this, bIsLoadTime);
    }
}

void UNavigationSystem::ConditionalPopulateNavOctree()
{
    if (Natives::UNavigationSystem_ConditionalPopulateNavOctree)
    {
        Natives::UNavigationSystem_ConditionalPopulateNavOctree(this);
    }
}

void UNavigationSystem::OnNavigationBoundsUpdated(ANavMeshBoundsVolume* NavVolume)
{
    Reflection::FParams Params(StaticFunction_OnNavigationBoundsUpdated());
    Params.Set(L"NavVolume", reinterpret_cast<UObject*>(NavVolume));
    Params.Call(this);
}

void UNavigationSystem::RequestRegistration(ANavigationData* NavData, bool bTriggerRegistrationProcessing)
{
    if (Natives::UNavigationSystem_RequestRegistration)
    {
        Natives::UNavigationSystem_RequestRegistration(this, NavData, bTriggerRegistrationProcessing);
    }
}

bool UNavigationSystem::InitializeForWorld(UWorld* World, FNavigationSystemRunMode::Type Mode)
{
    if (!Natives::UNavigationSystem_InitializeForWorld || !World)
    {
        return false;
    }
    Natives::UNavigationSystem_InitializeForWorld(World, Mode);
    return true;
}

bool FNavigationSystem::AddNavigationSystemToWorld(UWorld& WorldOwner, FNavigationSystemRunMode::Type RunMode, bool bInitializeForWorld)
{
    if (!Natives::FNavigationSystem_AddNavigationSystemToWorld)
    {
        return false;
    }
    Natives::FNavigationSystem_AddNavigationSystemToWorld(WorldOwner, RunMode, bInitializeForWorld);
    return true;
}

bool UNavigationSystem::IsNavigationBeingBuilt(UObject* WorldContextObject)
{
    Reflection::FParams Params(StaticFunction_IsNavigationBeingBuilt());
    Params.Set(Params.Has(L"WorldContextObject") ? L"WorldContextObject" : L"WorldContext", WorldContextObject);
    return Params.Call(StaticClass()->GetDefaultObject()) && Params.Take<bool>(L"ReturnValue");
}

void UNavigationSystem::RegisterNavigationInvoker(AActor* Invoker, float TileGenerationRadius, float TileRemovalRadius)
{
    Reflection::FParams Params(StaticFunction_RegisterNavigationInvoker(), true);
    Params.Set(L"Invoker", reinterpret_cast<UObject*>(Invoker));
    Params.Set(L"TileGenerationRadius", TileGenerationRadius);
    Params.Set(L"TileRemovalRadius", TileRemovalRadius);
    Params.Call(this);
}

bool UNavigationSystem::K2_ProjectPointToNavigation(UObject* WorldContextObject, const FVector& Point, FVector& ProjectedLocation, ANavigationData* NavData, const FVector& QueryExtent)
{
    Reflection::FParams Params(StaticFunction_K2_ProjectPointToNavigation(), true);
    Params.Set(L"WorldContextObject", WorldContextObject);
    Params.Set(L"Point", Point);
    Params.Set(L"NavData", reinterpret_cast<UObject*>(NavData));
    Params.Set(L"QueryExtent", QueryExtent);
    if (!Params.Call(StaticClass()->GetDefaultObject()) || !Params.Take<bool>(L"ReturnValue"))
    {
        return false;
    }
    ProjectedLocation = Params.Take<FVector>(L"ProjectedLocation");
    return true;
}
