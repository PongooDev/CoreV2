#include "pch.h"
#include "Athena/AI/AthenaNavSystem.h"
#include "AI/Navigation/NavMeshBoundsVolume.h"
#include "AI/Navigation/NavigationData.h"
#include "AI/Navigation/NavigationSystem.h"
#include "AI/Navigation/RecastNavMesh.h"
#include "Components/SceneComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Finder.h"
#include "GameFramework/WorldSettings.h"
#include "GameFramework/Volume.h"
#include "Log.h"
#include "PhysicsEngine/BodySetup.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"
#include "UObject/WeakObjectPtr.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace
{
    // 8.30's invoker tiles: the bot's surroundings (Core's radii 🟡; 21.40 keeps a 5000 uu removal radius on its boxes).
    constexpr float InvokerGenerationRadius = 4000.0f;
    constexpr float InvokerRemovalRadius = 6000.0f;
    // ENavDataGatheringModeConfig::Lazy and ERuntimeGenerationType::Dynamic.
    constexpr uint8_t GatheringLazy = 2;
    constexpr uint8_t RuntimeDynamic = 2;
    // Tile jobs at once (Core's 🟡): each one's geometry is gathered on the game thread when it starts (see
    // InitializeForBots), so this bounds that work per frame; the tiles still build on the worker threads.
    constexpr int32_t MaxTileGenerationJobs = 8;

    bool bInitialized = false;
    bool bReady = false;
    bool bTried = false;
    FWeakObjectPtr NavDataPtr;

    double NowMs()
    {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    std::wstring NameOf(const UObject* Object)
    {
        return Object ? Object->GetName().ToStdString() : std::wstring(L"none");
    }

    // A pointer member of Object, by its reflected name (a property Core has no wrapper for).
    UObject** ObjectMember(UObject* Object, const wchar_t* Name)
    {
        UProperty* Property = Object ? Object->GetClass()->FindPropertyByName(FName(Name, FNAME_Find)) : nullptr;
        return Property && Property->ElementSize == sizeof(UObject*) ? reinterpret_cast<UObject**>(reinterpret_cast<uint8_t*>(Object) + Property->GetOffset_ForInternal()) : nullptr;
    }

    // The agent a Battle Royale player is: a recast navmesh's agent (not STW's building-grid graph) whose radius is
    // nearest the player capsule's 🟡 (Fortnite's player capsule is 36 x 90 half-height).
    int32_t PickPlayerAgent(UNavigationSystem* NavSys)
    {
        static UClass* RecastClass = FindObject<UClass>(nullptr, L"/Script/Engine.RecastNavMesh");
        TStructArray<FNavDataConfig>& Agents = NavSys->SupportedAgents;
        int32_t Best = -1;
        float BestScore = 1.0e9f;
        for (int32_t Index = 0; Index < Agents.Num(); ++Index)
        {
            FNavDataConfig& Agent = Agents[Index];
            UClass* DataClass = Agent.NavigationDataClass;
            const float Radius = Agent.AgentRadius;
            Log::Info(L"Battle Royale navigation: agent {} {}: {}, radius {:.0f}, height {:.0f}", Index, static_cast<FName>(Agent.Name).ToString().ToStdString(), NameOf(DataClass), Radius, static_cast<float>(Agent.AgentHeight));
            if (!DataClass || !RecastClass || !DataClass->IsChildOf(RecastClass))
            {
                continue;
            }
            const float Score = std::fabs(Radius - 36.0f);
            if (Score < BestScore)
            {
                BestScore = Score;
                Best = Index;
            }
        }
        return Best;
    }

    // One NavMeshBoundsVolume over the island. Battle Royale's cook strips the brushes, and a spawned volume has none:
    // its brush component's bounds come from a one-box body setup instead (UBrushComponent::CalcBounds uses
    // BrushBodySetup without a brush), a box the island's size.
    ANavMeshBoundsVolume* SpawnIslandBounds(UWorld* World, const FBox& Island)
    {
        FActorSpawnParameters SpawnInfo;
        SpawnInfo.ObjectFlags = SpawnInfo.ObjectFlags | RF_Transient;
        SpawnInfo.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        ANavMeshBoundsVolume* Volume = World->SpawnActor<ANavMeshBoundsVolume>(ANavMeshBoundsVolume::StaticClass(), SpawnInfo);
        UPrimitiveComponent* BrushComponent = Volume ? reinterpret_cast<AVolume*>(Volume)->GetBrushComponent() : nullptr;
        UObject** BodySetupMember = BrushComponent ? ObjectMember(reinterpret_cast<UObject*>(BrushComponent), L"BrushBodySetup") : nullptr;
        if (!BodySetupMember)
        {
            Log::Warning(L"Battle Royale navigation: the bounds volume has no brush component to shape");
            return Volume;
        }
        UBodySetup* BodySetup = NewObject<UBodySetup>(reinterpret_cast<UObject*>(BrushComponent), UBodySetup::StaticClass());
        if (BodySetup)
        {
            FKBoxElem& Elem = BodySetup->AggGeom.BoxElems.AddDefaulted();
            // The box is the island's size at scale 1: FKAggregateGeom::CalcAABB scales box elements by the smallest
            // axis of the component's scale (the first run's island-scaled unit box came out a 22440 cube).
            const FVector Size = Island.Max - Island.Min;
            Elem.X = Size.X;
            Elem.Y = Size.Y;
            Elem.Z = Size.Z;
            *BodySetupMember = reinterpret_cast<UObject*>(BodySetup);
        }
        if (USceneComponent* Root = Volume->GetRootComponent())
        {
            Root->Mobility = EComponentMobility::Movable;
        }
        Volume->SetActorLocation(Island.GetCenter());
        return Volume;
    }

    // 3.0's Athena world is made without a navigation system: its world settings turn bEnableNavigationSystem off, so
    // UWorld::InitWorld and LoadMap's UNavigationSystem::InitializeForWorld skip CreateNavigationSystem. 8.30's Athena
    // world has one (UAthenaNavSystem, from its world settings and DefaultEngine's NavigationSystemClass). Core turns
    // the world's setting on and runs the engine's own InitializeForWorld(World, GameMode) as LoadMap does: it makes
    // GEngine's NavigationSystemClass for the world and runs its OnWorldInitDone. The class default's config is
    // 8.30's UAthenaNavSystemConfig for the new system's lifetime (invokers only, no data spawned for every agent, lazy
    // gathering), so the system is born with it, as from DefaultEngine.ini; the default is put back afterwards.
    UNavigationSystem* CreateNavigationSystem(UWorld* World)
    {
        AWorldSettings* Settings = World->GetWorldSettings(false, false);
        UClass* NavSysClass = GEngine->NavigationSystemClass;
        UNavigationSystem* Default = NavSysClass ? reinterpret_cast<UNavigationSystem*>(NavSysClass->GetDefaultObject()) : nullptr;
        if (!Settings || !Default || !Finder::ResolveNavigationSystemInitializeForWorldLive(UWorld::NavigationSystemOffset()))
        {
            Log::Warning(L"Battle Royale navigation: can't make the world's navigation system (settings {}, class {})", NameOf(reinterpret_cast<UObject*>(Settings)), NameOf(reinterpret_cast<UObject*>(NavSysClass)));
            return nullptr;
        }
        const bool bWasEnabled = Settings->bEnableNavigationSystem;
        const bool bWasInvokersOnly = Default->bGenerateNavigationOnlyAroundNavigationInvokers;
        const bool bWasAutoCreate = Default->bAutoCreateNavigationData;
        const uint8_t WasGathering = Default->DataGatheringMode;
        Settings->bEnableNavigationSystem = true;
        Default->bGenerateNavigationOnlyAroundNavigationInvokers = true;
        Default->bAutoCreateNavigationData = false;
        Default->DataGatheringMode = GatheringLazy;
        const double Started = NowMs();
        UNavigationSystem::InitializeForWorld(World, FNavigationSystemRunMode::GameMode);
        Default->bGenerateNavigationOnlyAroundNavigationInvokers = bWasInvokersOnly;
        Default->bAutoCreateNavigationData = bWasAutoCreate;
        Default->DataGatheringMode = WasGathering;
        UNavigationSystem* NavSys = World->GetNavigationSystem();
        Log::Info(L"Battle Royale navigation: the world's navigation system was {} (world settings had navigation {}); made {} in {:.0f} ms", NavSys ? L"made" : L"not made",
            bWasEnabled ? L"on" : L"off", NameOf(reinterpret_cast<UObject*>(NavSys)), NowMs() - Started);
        return NavSys;
    }
}

bool UAthenaNavSystem::IsInitialized()
{
    return bInitialized;
}

void UAthenaNavSystem::ResetForNewMatch()
{
    bInitialized = false;
    bReady = false;
    bTried = false;
    NavDataPtr = FWeakObjectPtr();
}

bool UAthenaNavSystem::InitializeForBots(UWorld* World, const FBox& IslandBounds)
{
#if WITH_SERVER_CODE
    if (bTried)
    {
        return bInitialized;
    }
    bTried = true;
    UNavigationSystem* NavSys = World ? World->GetNavigationSystem() : nullptr;
    if (!NavSys && World && IslandBounds.IsValid)
    {
        NavSys = CreateNavigationSystem(World);
    }
    if (!NavSys || !IslandBounds.IsValid || !UNavigationSystem::StaticFunction_RegisterNavigationInvoker())
    {
        Log::Warning(L"Battle Royale navigation: the world has no navigation system ({}), the bots steer without a navmesh", NameOf(reinterpret_cast<UObject*>(NavSys)));
        return false;
    }
    Log::Info(L"Battle Royale navigation: {} has {} nav data (main {}), {} agents, invokers only {}, auto create {}, gathering {}, build lock 0x{:X}", NameOf(reinterpret_cast<UObject*>(NavSys)),
        static_cast<TArray<ANavigationData*>&>(NavSys->NavDataSet).Num(), NameOf(reinterpret_cast<UObject*>(static_cast<ANavigationData*>(NavSys->MainNavData))), NavSys->SupportedAgents.Num(),
        NavSys->bGenerateNavigationOnlyAroundNavigationInvokers, NavSys->bAutoCreateNavigationData, static_cast<int32_t>(NavSys->DataGatheringMode), static_cast<int32_t>(NavSys->NavBuildingLockFlags));

    const int32_t AgentIndex = PickPlayerAgent(NavSys);
    if (AgentIndex < 0)
    {
        Log::Warning(L"Battle Royale navigation: no recast navmesh agent, the bots steer without a navmesh");
        return false;
    }
    FNavDataConfig& Agent = NavSys->SupportedAgents[AgentIndex];
    UClass* DataClass = Agent.NavigationDataClass;

    // 8.30's UAthenaNavSystemConfig: bUseNavigationInvokers and bLazyOctree. Set before any navmesh makes its generator
    // (FRecastNavMeshGenerator reads the invoker flag when it is made), and no other agent's data is spawned by itself.
    NavSys->bGenerateNavigationOnlyAroundNavigationInvokers = true;
    NavSys->bAutoCreateNavigationData = false;
    NavSys->DataGatheringMode = GatheringLazy;

    const double Started = NowMs();
    ANavMeshBoundsVolume* Volume = SpawnIslandBounds(World, IslandBounds);
    FVector Origin;
    FVector Extent;
    if (Volume)
    {
        Volume->GetActorBounds(false, Origin, Extent);
        const FVector Scale = Volume->GetActorScale3D();
        Log::Info(L"Battle Royale navigation: island ({:.0f}, {:.0f}, {:.0f}) - ({:.0f}, {:.0f}, {:.0f}); bounds volume at ({:.0f}, {:.0f}, {:.0f}), extent ({:.0f}, {:.0f}, {:.0f}), scale ({:.0f}, {:.0f}, {:.0f})",
            IslandBounds.Min.X, IslandBounds.Min.Y, IslandBounds.Min.Z, IslandBounds.Max.X, IslandBounds.Max.Y, IslandBounds.Max.Z, Origin.X, Origin.Y, Origin.Z, Extent.X, Extent.Y, Extent.Z, Scale.X, Scale.Y, Scale.Z);
        NavSys->OnNavigationBoundsUpdated(Volume);
    }
    if (!Volume || Extent.X < 1000.0f || Extent.Y < 1000.0f)
    {
        Log::Warning(L"Battle Royale navigation: the island's bounds volume has no size, the bots steer without a navmesh");
        return false;
    }

    // The navmesh, as AFortNavigationGraph::FindOrSpawn spawns the Zone's graph: the agent's class and config, made
    // dynamic (the runtime tiles need a generator), then registered.
    FActorSpawnParameters SpawnInfo;
    SpawnInfo.ObjectFlags = SpawnInfo.ObjectFlags | RF_Transient;
    SpawnInfo.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    ANavigationData* NavData = World->SpawnActor<ANavigationData>(DataClass, SpawnInfo);
    if (!NavData)
    {
        Log::Warning(L"Battle Royale navigation: couldn't spawn {}", NameOf(DataClass));
        return false;
    }
    FNavDataConfig::StaticStruct()->CopyScriptStruct(&NavData->NavDataConfig, &Agent);
    if (ANavigationData::HasRuntimeGeneration())
    {
        Log::Info(L"Battle Royale navigation: {} runtime generation {} -> dynamic", NameOf(NavData), static_cast<int32_t>(NavData->RuntimeGeneration));
        NavData->RuntimeGeneration = RuntimeDynamic;
    }
    // Each tile's geometry gathered on the game thread as its job is made (FRecastTileGenerator::Setup's GatherGeometry,
    // the engine's default), the build itself still on a worker thread, a few jobs at once. FortNavMesh gathers it on the tile
    // jobs' worker threads (bDoFullyAsyncNavDataGathering), and with lazy gathering a landscape component's slice for a
    // tile is written into the component's one shared FNavigationRelevantData (3.1's DoAsyncGeometryGathering 0x1E3F3B0:
    // GatherGeometrySlice, StoreCollisionCache appending to its CollisionData, then emptying it). Two jobs over the same
    // component (any two neighbouring tiles) race on it: the second append breaks the first's header, and
    // FNavigationRelevantData::IsCollisionDataValid (0x1E1D190) logs "NavOctree element has corrupted collision data!
    // Owner:LandscapeHeightfieldCollisionComponent_N Bounds:IsValid=false" (402 times in one 3.1 match) and the tile is
    // built without the ground. A tile emptied by the other job before its append loses the ground without the error.
    if (ARecastNavMesh::HasTileGenerationSettings())
    {
        ARecastNavMesh* RecastNavMesh = reinterpret_cast<ARecastNavMesh*>(NavData);
        const bool bWasAsync = RecastNavMesh->bDoFullyAsyncNavDataGathering;
        const int32_t WasJobs = RecastNavMesh->MaxSimultaneousTileGenerationJobsCount;
        RecastNavMesh->bDoFullyAsyncNavDataGathering = false;
        RecastNavMesh->MaxSimultaneousTileGenerationJobsCount = std::clamp(WasJobs, 1, MaxTileGenerationJobs);
        Log::Info(L"Battle Royale navigation: {} gathers tile geometry on the game thread (was {}), {} tile jobs at once (was {})", NameOf(NavData),
            bWasAsync ? L"on the jobs' threads" : L"on the game thread", static_cast<int32_t>(RecastNavMesh->MaxSimultaneousTileGenerationJobsCount), WasJobs);
    }
    else
    {
        Log::Warning(L"Battle Royale navigation: {} has no tile generation settings; its landscape tiles may be built without the ground", NameOf(NavData));
    }
    NavSys->RequestRegistration(NavData, true);
    NavDataPtr = FWeakObjectPtr(reinterpret_cast<UObject*>(NavData));
    Log::Info(L"Battle Royale navigation: {} for agent {} over the island, registering ({:.0f} ms)", NameOf(NavData), AgentIndex, NowMs() - Started);
    bInitialized = true;
    return true;
#else
    return false;
#endif
}

void UAthenaNavSystem::Tick(UWorld* World)
{
#if WITH_SERVER_CODE
    // The registration runs on the game thread's next task pass (RequestRegistration queues ProcessRegistrationCandidates).
    // Once the navmesh is registered (and so needs the octree: RequiresNavOctree), the octree (the geometry the tiles are
    // built from, gathered lazily) is populated, and the initial lock released: its RebuildAll only marks the active
    // tiles, which are the invokers'.
    if (!bInitialized || bReady)
    {
        return;
    }
    UNavigationSystem* NavSys = World ? World->GetNavigationSystem() : nullptr;
    ANavigationData* NavData = GetNavData(World);
    if (!NavSys || !NavData)
    {
        return;
    }
    bReady = true;
    NavSys->SetNavigationOctreeLock(false);
    const double OctreeStarted = NowMs();
    NavSys->ConditionalPopulateNavOctree();
    const double OctreeMs = NowMs() - OctreeStarted;
    const double LockStarted = NowMs();
    NavSys->ReleaseInitialBuildingLock();
    Log::Info(L"Battle Royale navigation: {} registered (main {}); octree {:.0f} ms, lock release {:.0f} ms, build lock 0x{:X}", NameOf(NavData),
        NameOf(reinterpret_cast<UObject*>(static_cast<ANavigationData*>(NavSys->MainNavData))), OctreeMs, NowMs() - LockStarted, static_cast<int32_t>(NavSys->NavBuildingLockFlags));
#endif
}

ANavigationData* UAthenaNavSystem::GetNavData(UWorld* World)
{
    ANavigationData* NavData = static_cast<ANavigationData*>(NavDataPtr.Get());
    UNavigationSystem* NavSys = World ? World->GetNavigationSystem() : nullptr;
    if (!NavData || !NavSys)
    {
        return nullptr;
    }
    TArray<ANavigationData*>& Registered = NavSys->NavDataSet;
    for (ANavigationData* Data : Registered)
    {
        if (Data == NavData)
        {
            return NavData;
        }
    }
    return nullptr;
}

void UAthenaNavSystem::RegisterNavigationInvoker(AActor* Invoker)
{
    UWorld* World = Invoker && bInitialized ? Invoker->GetWorld() : nullptr;
    if (UNavigationSystem* NavSys = World ? World->GetNavigationSystem() : nullptr)
    {
        NavSys->RegisterNavigationInvoker(Invoker, InvokerGenerationRadius, InvokerRemovalRadius);
    }
}

bool UAthenaNavSystem::ProjectPointToNavigation(UWorld* World, const FVector& Point, FVector& OutLocation, const FVector& Extent)
{
    ANavigationData* NavData = GetNavData(World);
    return NavData && UNavigationSystem::K2_ProjectPointToNavigation(reinterpret_cast<UObject*>(World), Point, OutLocation, NavData, Extent);
}

bool UAthenaNavSystem::IsOnNavMesh(UWorld* World, const FVector& Location)
{
    FVector Projected;
    return ProjectPointToNavigation(World, Location, Projected, FVector(100.0f, 100.0f, 250.0f));
}
