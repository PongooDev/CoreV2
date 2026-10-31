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

bool UNavigationSystem::IsNavigationBeingBuilt(UObject* WorldContextObject)
{
    Reflection::FParams Params(StaticFunction_IsNavigationBeingBuilt());
    Params.Set(Params.Has(L"WorldContextObject") ? L"WorldContextObject" : L"WorldContext", WorldContextObject);
    return Params.Call(StaticClass()->GetDefaultObject()) && Params.Take<bool>(L"ReturnValue");
}
