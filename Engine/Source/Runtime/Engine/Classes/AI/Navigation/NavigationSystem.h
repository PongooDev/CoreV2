#pragma once

#include "AI/Navigation/NavigationData.h"
#include "ObjectLayout.h"
#include "Reflection.h"
#include "StructArray.h"
#include "UObject/Object.h"

namespace ENavigationBuildLock
{
    enum Type
    {
        NoUpdateInEditor = 1 << 1,
        InitialLock = 1 << 2,
        Custom = 1 << 3,
    };
}

class ANavMeshBoundsVolume;

class UNavigationSystem : public UObject
{
    DefineUnrealClass(UNavigationSystem, Engine)
    DefineNativeMember(uint8_t, NavBuildingLockFlags, UNavigationSystem_NavBuildingLockFlags)
    DefineNativeMember(uint8_t, NavOctreeLockFlags, UNavigationSystem_bNavOctreeLock)
    DefineUFunction(OnNavigationBoundsUpdated)
    DefineUStructArrayProperty(FNavDataConfig, SupportedAgents)

public:
    bool IsNavigationBuildingLocked() const { return NavBuildingLockFlags != 0; }
    void RemoveNavigationBuildLock(uint8_t Flags);
    void ReleaseInitialBuildingLock();
    void RebuildAll(bool bIsLoadTime = false);
    bool IsNavigationOctreeLocked() const { return (NavOctreeLockFlags & 1) != 0; }
    void SetNavigationOctreeLock(bool bLock) { NavOctreeLockFlags = static_cast<uint8_t>((NavOctreeLockFlags & ~1) | (bLock ? 1 : 0)); }
    void ConditionalPopulateNavOctree();
    void OnNavigationBoundsUpdated(ANavMeshBoundsVolume* NavVolume);
    void RequestRegistration(ANavigationData* NavData, bool bTriggerRegistrationProcessing = true);
};
