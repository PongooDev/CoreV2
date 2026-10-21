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

void UNavigationSystem::OnNavigationBoundsUpdated(ANavMeshBoundsVolume* NavVolume)
{
    Reflection::FParams Params(StaticFunction_OnNavigationBoundsUpdated());
    Params.Set(L"NavVolume", reinterpret_cast<UObject*>(NavVolume));
    Params.Call(this);
}
