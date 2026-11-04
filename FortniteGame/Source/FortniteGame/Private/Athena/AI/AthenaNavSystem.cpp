#include "pch.h"
#include "Athena/AI/AthenaNavSystem.h"
#include "AI/Navigation/NavMeshBoundsVolume.h"
#include "AI/Navigation/NavigationData.h"
#include "AI/Navigation/NavigationSystem.h"
#include "Components/SceneComponent.h"
#include "Engine/World.h"
#include "GameFramework/Volume.h"
#include "Log.h"
#include "PhysicsEngine/BodySetup.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"
#include "UObject/WeakObjectPtr.h"

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

    bool bInitialized = false;
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
            Log::Info(L"Battle Royale navigation: agent {} {}: {}, radius {:.0f}, height {:.0f}", Index, static_cast<FName>(Agent.Name).ToString(), NameOf(DataClass), Radius, static_cast<float>(Agent.AgentHeight));
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
    // BrushBodySetup without a brush), scaled to the island like the Zone's unit volumes (FortGameModeZone.cpp).
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
            Elem.X = 1.0f;
            Elem.Y = 1.0f;
            Elem.Z = 1.0f;
            *BodySetupMember = reinterpret_cast<UObject*>(BodySetup);
        }
        if (USceneComponent* Root = Volume->GetRootComponent())
        {
            Root->Mobility = EComponentMobility::Movable;
        }
        Volume->SetActorScale3D(Island.Max - Island.Min);
        Volume->SetActorLocation(Island.GetCenter());
        return Volume;
    }
}

bool UAthenaNavSystem::IsInitialized()
{
    return bInitialized;
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
    if (!NavSys || !IslandBounds.IsValid || !UNavigationSystem::StaticFunction_RegisterNavigationInvoker())
    {
        // 3.0's Athena world is made without one (UWorld::NavigationSystem is null: the engine only creates it for a
        // world whose settings enable navigation), and making one needs the engine's native, unreflected
        // CreateNavigationSystem / InitializeForWorld.
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
        Log::Info(L"Battle Royale navigation: bounds volume at ({:.0f}, {:.0f}, {:.0f}), extent ({:.0f}, {:.0f}, {:.0f})", Origin.X, Origin.Y, Origin.Z, Extent.X, Extent.Y, Extent.Z);
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
    NavSys->RequestRegistration(NavData, true);
    NavDataPtr = FWeakObjectPtr(reinterpret_cast<UObject*>(NavData));

    // The octree (the geometry the tiles are built from, gathered lazily), and the initial lock: its RebuildAll only
    // marks the active tiles, of which there are none until the bots register.
    NavSys->SetNavigationOctreeLock(false);
    const double OctreeStarted = NowMs();
    NavSys->ConditionalPopulateNavOctree();
    const double OctreeMs = NowMs() - OctreeStarted;
    const double LockStarted = NowMs();
    NavSys->ReleaseInitialBuildingLock();
    Log::Info(L"Battle Royale navigation: {} for agent {} over the island; octree {:.0f} ms, lock release {:.0f} ms, total {:.0f} ms", NameOf(NavData), AgentIndex, OctreeMs, NowMs() - LockStarted, NowMs() - Started);
    bInitialized = true;
    return true;
#else
    return false;
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

bool UAthenaNavSystem::IsOnNavMesh(UWorld* World, const FVector& Location)
{
    ANavigationData* NavData = GetNavData(World);
    FVector Projected;
    return NavData && UNavigationSystem::K2_ProjectPointToNavigation(reinterpret_cast<UObject*>(World), Location, Projected, NavData, FVector(100.0f, 100.0f, 250.0f));
}
