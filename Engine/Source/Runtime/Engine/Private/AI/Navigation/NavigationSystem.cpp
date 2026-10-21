#include "pch.h"
#include "AI/Navigation/NavigationSystem.h"
#include "Natives.h"

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
