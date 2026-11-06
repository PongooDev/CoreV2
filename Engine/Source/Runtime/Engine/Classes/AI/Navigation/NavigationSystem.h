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
class UWorld;

namespace FNavigationSystemRunMode
{
    enum Type : uint8_t
    {
        InvalidMode,
        GameMode,
        EditorMode,
        SimulationMode,
        PIEMode,
    };
}

class UNavigationSystem : public UObject
{
    // 4.2 takes UE 4.20's NavigationSystem module: the class is UNavigationSystemV1 there (Engine keeps a stub
    // UNavigationSystem with none of its members).
    DefineMovedRenamedUnrealClass(UNavigationSystem, Engine, UNavigationSystemV1, NavigationSystem)
    DefineNativeMember(uint8_t, NavBuildingLockFlags, UNavigationSystem_NavBuildingLockFlags)
    DefineNativeMember(uint8_t, NavOctreeLockFlags, UNavigationSystem_bNavOctreeLock)
    DefineUFunction(OnNavigationBoundsUpdated)
    DefineUFunction(IsNavigationBeingBuilt)
    DefineUFunction(RegisterNavigationInvoker)
    DefineUFunction(K2_ProjectPointToNavigation)
    DefineUStructArrayProperty(FNavDataConfig, SupportedAgents)
    DefineUProperty(ANavigationData*, MainNavData)
    DefineUProperty(TArray<ANavigationData*>, NavDataSet)
    // Navigation generated only in tiles around the registered invokers (read when a navmesh makes its generator), and
    // whether the system spawns the agents' navigation data by itself once there are navigation bounds.
    DefineBitfieldUProperty(bGenerateNavigationOnlyAroundNavigationInvokers)
    DefineBitfieldUProperty(bAutoCreateNavigationData)
    // ENavDataGatheringModeConfig: 1 Instant, 2 Lazy (geometry gathered when a tile is built).
    DefineUProperty(uint8_t, DataGatheringMode)

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

    // An actor around which navigation is generated (TileGenerationRadius) and kept (TileRemovalRadius), while it lives.
    void RegisterNavigationInvoker(AActor* Invoker, float TileGenerationRadius, float TileRemovalRadius);

    static bool IsNavigationBeingBuilt(UObject* WorldContextObject);
    // UEngine::LoadMap's call once the world's actors are initialized: CreateNavigationSystem when the world has no
    // navigation system (GEngine's NavigationSystemClass, if the world settings' bEnableNavigationSystem is on), then
    // its OnWorldInitDone(Mode). False when the engine function wasn't found.
    static bool InitializeForWorld(UWorld* World, FNavigationSystemRunMode::Type Mode);
    // The point on the navigation data within QueryExtent of Point, or false when there is none there (no tile yet).
    static bool K2_ProjectPointToNavigation(UObject* WorldContextObject, const FVector& Point, FVector& ProjectedLocation, ANavigationData* NavData, const FVector& QueryExtent);
};
