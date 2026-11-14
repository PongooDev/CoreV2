#include "pch.h"
#include "FortGameModeZone.h"
#include "Athena/AI/CoreAthenaBotManager.h"
#include "Athena/FortGameModeAthena.h"
#include "Athena/FortPoiManager.h"
#include "Athena/FortPoiVolume.h"
#include "FortGameModeOutpost.h"
#include "Player/FortPlayerStateOutpost.h"
#include "AI/FortAIDirector.h"
#include "AI/FortAIGoalManager.h"
#include "AI/Navigation/FortNavigationGraph.h"
#include "AI/Navigation/NavigationSystem.h"
#include "AI/Navigation/NavMeshBoundsVolume.h"
#include "Components/SceneComponent.h"
#include "Engine/World.h"
#include "BlueprintGameplayTagLibrary.h"
#include "EnvironmentQuery/EnvQueryInstanceBlueprintWrapper.h"
#include "EnvironmentQuery/EnvQueryManager.h"
#include "FortAssetManager.h"
#include "FortGameData.h"
#include "FortGameState.h"
#include "FortVisibilityManager.h"
#include "FortZoneTheme.h"
#include "DeployableBases/FortDeployableBaseManager.h"
#include "UI/FrontEnd/FortMissionGenerator.h"
#include "GameFramework/OnlineReplStructs.h"
#include "Online/FortLobbyBeaconHost.h"
#include "Player/FortPlayerController.h"
#include "Player/FortPlayerState.h"
#include "FortWorldManager.h"
#include "GameThread.h"
#include "Logging/LogMacros.h"
#include "Building/BuildingActor.h"
#include "Building/BuildingContainer.h"
#include "Building/BuildingStructuralSupportSystem.h"
#include "Configuration.h"
#include "GameFramework/Character.h"
#include "Kismet/GameplayStatics.h"
#include "Pawns/FortPawn.h"
#include "ReflectionParams.h"
#include "Templates/Casts.h"
#include "TimerManager.h"
#include "UObject/WeakObjectPtr.h"
#include "Finder.h"
#include "Hooks.h"
#include "Log.h"
#include "Natives.h"
#include "ObjectLayout.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

DEFINE_LOG_CATEGORY_STATIC(LogFort, Log, All);
DEFINE_LOG_CATEGORY_STATIC(LogFortPlacement, Log, All);
DEFINE_LOG_CATEGORY_STATIC(LogNavigation, Log, All);

namespace
{
    // AFortGameMode::SpawnDefaultPawnFor_Implementation's rule for a spot someone is standing on (6.02 0xE5CD63, 6.20
    // 0xEF1DA3): another player controller's live AFortPawn less than 6 units from it, across the ground (X and Y).
    constexpr float SpawnSpotTakenDistanceSquared = 36.0f;
    // How far above the start spot that spawn puts the pawn, over its half height (6.02 0xE5CA21's constant).
    constexpr float SpawnHeightAboveSpot = 24.0f;
    // The player sockets of the pad's mesh, SpawnPads' Player1-4, where the pad has no MaximumNumberOfPlayers of its own.
    constexpr int32_t DefaultSpawnPadPlayers = 4;

    bool IsSpawnSpotTaken(const std::vector<FVector>& PlayerLocations, const FVector& Spot)
    {
        return std::any_of(PlayerLocations.begin(), PlayerLocations.end(), [&Spot](const FVector& Location)
        {
            const float DX = Location.X - Spot.X;
            const float DY = Location.Y - Spot.Y;
            return DX * DX + DY * DY < SpawnSpotTakenDistanceSquared;
        });
    }
}

APawn* AFortGameModeZone::SpawnDefaultPawnFor_Implementation(AController* NewPlayer, AActor* StartSpot)
{
#if WITH_SERVER_CODE
    if (!StartSpot || !NewPlayer)
    {
        return nullptr;
    }

    // The client builds strip the zone's override down to its first checks (6.02 0xE5D050: the start spot, the player,
    // GetDefaultPawnClassForController, the AFortPlayerController cast, GetWorld, then return null). 21.40 names what
    // it had for the spawn pad, GetLocationAndRotationForSpawnPad and TrySpawnDefaultPawn, but no build keeps their
    // bodies. Core's reading, for the spawn pad only: the pad's own spot while no other player stands on it, as
    // AFortGameMode's spawn gives; with someone on it, a free player socket of the same pad, before AFortGameMode's spawn
    // turns the pad down and ChoosePlayerStart falls back to the backup spots (on the Canny Valley outpost those are the
    // husk spawners in the canyon, outside the storm shield: #423).
    if (Member_ActiveSpawnPad.IsAvailable() && StartSpot == static_cast<AActor*>(ActiveSpawnPad) && AFortPlayerController::HasSpawnLoc() && AFortPlayerState::CanSetHomeActor())
    {
        FVector Location;
        FRotator Rotation;
        if (GetLocationAndRotationForSpawnPad(NewPlayer, Location, Rotation))
        {
            if (APawn* Pawn = TrySpawnDefaultPawn(NewPlayer, StartSpot, Location, Rotation))
            {
                return Pawn;
            }
            UE_LOG(LogFortPlacement, Warning, TEXT("SpawnDefaultPawnFor: couldn't spawn %s on the spawn pad's free spot, trying the game's spots"), *NewPlayer->GetName());
        }
    }

    return AFortGameMode::SpawnDefaultPawnFor_Implementation(NewPlayer, StartSpot);
#else
    return nullptr;
#endif
}

bool AFortGameModeZone::GetLocationAndRotationForSpawnPad(AController* Player, FVector& OutLocation, FRotator& OutRotation)
{
#if WITH_SERVER_CODE
    // The pad Blueprint's own placement of its players (BP_PlayerSpawnTeleportPad: MaximumNumberOfPlayers 4, and
    // GetPlayerTeleportTransform(PlayerIndex), the world transform of its mesh's "Player<PlayerIndex + 1>" socket, which
    // its TeleportAllPlayers puts each player on at the mission's end). A pad without it keeps the game's spawn.
    AActor* SpawnPad = Member_ActiveSpawnPad.IsAvailable() ? static_cast<AActor*>(ActiveSpawnPad) : nullptr;
    UFunction* GetPlayerTeleportTransform = SpawnPad ? SpawnPad->FindFunction(FName(L"GetPlayerTeleportTransform", FNAME_Find)) : nullptr;
    UWorld* World = GetWorld();
    if (!GetPlayerTeleportTransform || !World)
    {
        return false;
    }

    // Where the other players are, as AFortGameMode's spawn checks them: their controllers' live AFortPawns.
    std::vector<FVector> PlayerLocations;
    for (TObjectIterator<AFortPlayerController> It; It; ++It)
    {
        AFortPlayerController* Other = *It;
        if (Other == Player || Other->GetWorld() != World || Other->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
        {
            continue;
        }
        AFortPawn* Pawn = Cast<AFortPawn>(reinterpret_cast<UObject*>(static_cast<APawn*>(Other->Pawn)));
        if (Pawn && !Pawn->IsDead())
        {
            PlayerLocations.push_back(Pawn->GetTransform().GetLocation());
        }
    }

    // The pad's own spot is free: AFortGameMode's spawn puts the player there, as it always has.
    const FVector PadSpot = SpawnPad->GetTransform().GetLocation();
    if (!IsSpawnSpotTaken(PlayerLocations, PadSpot))
    {
        return false;
    }

    // bUseAllSocketsInSpawnPad (true only on BP_DeployableBaseGameMode, Challenge the Horde) is left alone: its readers
    // are stripped with the rest, and what Epic did with it false (every other game mode, the outposts too) isn't known.
    const UProperty* MaximumNumberOfPlayers = SpawnPad->GetClass()->FindPropertyByName(FName(L"MaximumNumberOfPlayers", FNAME_Find));
    const bool bHasMaximum = MaximumNumberOfPlayers && MaximumNumberOfPlayers->GetClass()->HasAnyCastFlag(CASTCLASS_UIntProperty) && MaximumNumberOfPlayers->GetSize() == sizeof(int32_t);
    const int32_t NumPlayerSpots = bHasMaximum ? *MaximumNumberOfPlayers->ContainerPtrToValuePtr<int32_t>(SpawnPad) : DefaultSpawnPadPlayers;
    for (int32_t PlayerIndex = 0; PlayerIndex < std::min(NumPlayerSpots, 16); ++PlayerIndex)
    {
        Reflection::FParams Params(GetPlayerTeleportTransform, true);
        Params.Set(L"PlayerIndex", PlayerIndex);
        if (!Params.Call(SpawnPad))
        {
            return false;
        }
        const FTransform TeleportTransform = Params.Take<FTransform>(L"Teleport Transform");
        const FVector Spot = TeleportTransform.GetLocation();
        // A socket the mesh doesn't have is the mesh's own transform (USceneComponent::GetSocketTransform).
        const FVector FromPad = Spot - PadSpot;
        if (FromPad.X * FromPad.X + FromPad.Y * FromPad.Y + FromPad.Z * FromPad.Z < 1.0f || IsSpawnSpotTaken(PlayerLocations, Spot))
        {
            continue;
        }
        OutLocation = Spot;
        OutRotation = FRotator(0.0f, TeleportTransform.Rotation.Rotator().Yaw, 0.0f);
        UE_LOG(LogFortPlacement, Log, TEXT("SpawnDefaultPawnFor: a player stands on %s, %s spawns on its player spot %d"), *SpawnPad->GetName(), Player ? *Player->GetName() : TEXT("None"), PlayerIndex + 1);
        return true;
    }
    UE_LOG(LogFortPlacement, Log, TEXT("SpawnDefaultPawnFor: %s and all %d of its player spots are taken"), *SpawnPad->GetName(), NumPlayerSpots);
#endif
    return false;
}

APawn* AFortGameModeZone::TrySpawnDefaultPawn(AController* NewPlayer, AActor* StartSpot, const FVector& Location, const FRotator& Rotation)
{
#if WITH_SERVER_CODE
    // AFortGameMode::SpawnDefaultPawnFor_Implementation's spawn at one spot (6.02 0xE5C890, 6.20 0xEF18D0), at the given
    // place instead of the start spot's: the controller's SpawnLoc raised by the pawn's default half height and 24 (Core
    // reads that half height as ACharacter::GetDefaultHalfHeight does, from the class default's capsule), the start spot
    // bound as the player's home actor while they have none, the pawn spawned with the game mode's instigator and
    // AdjustIfPossibleButDontSpawnIfColliding, facing the spot's yaw, and bFailedToRespawn set from the result.
    AFortPlayerController* PlayerController = Cast<AFortPlayerController>(reinterpret_cast<UObject*>(NewPlayer));
    UClass* PawnClass = PlayerController ? GetDefaultPawnClassForController(NewPlayer) : nullptr;
    UWorld* World = PawnClass ? GetWorld() : nullptr;
    AFortPlayerState* PlayerState = PlayerController ? Cast<AFortPlayerState>(reinterpret_cast<UObject*>(static_cast<APlayerState*>(PlayerController->PlayerState))) : nullptr;
    if (!World || !AFortPlayerController::HasSpawnLoc() || !AFortPlayerState::CanSetHomeActor())
    {
        return nullptr;
    }

    const ACharacter* PawnDefault = Cast<ACharacter>(PawnClass->GetDefaultObject(false));
    const float HalfHeight = PawnDefault ? PawnDefault->GetDefaultHalfHeight() : 0.0f;
    const FVector SpawnLoc(Location.X, Location.Y, Location.Z + HalfHeight + SpawnHeightAboveSpot);
    PlayerController->SpawnLoc = SpawnLoc;

    if (PlayerState && !static_cast<AActor*>(PlayerState->HomeActor))
    {
        PlayerState->SetHomeActor(StartSpot);
    }

    const float HalfYaw = Rotation.Yaw * (3.1415926535897932f / 360.0f);
    FTransform SpawnTransform;
    SpawnTransform.Rotation.Z = std::sin(HalfYaw);
    SpawnTransform.Rotation.W = std::cos(HalfYaw);
    SpawnTransform.SetLocation(SpawnLoc);

    FActorSpawnParameters SpawnInfo;
    SpawnInfo.Instigator = Instigator;
    SpawnInfo.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButDontSpawnIfColliding;
    APawn* Pawn = Cast<APawn>(World->SpawnActor(PawnClass, &SpawnTransform, SpawnInfo));
    PlayerController->bFailedToRespawn = Pawn == nullptr;
    return Pawn;
#else
    return nullptr;
#endif
}

namespace
{
    // The spawn pad's placement query is asynchronous: UEnvQueryManager runs it on its tick over the next frames. Epic's
    // PlacePlayerSpawnPad ran it as an instant query (21.40's AFortGameModeZone binds no query completion delegate:
    // UEnvQueryManager::RunInstantQuery), which the client builds don't link. So the game mode waits for the async
    // query's own completion, as the game's blueprints wait for theirs (5.30's Trap_Floor_Defender_Master binds its
    // EQS_DefenderPadTeleportIn query's OnQueryFinishedEvent): the wrapper broadcasts OnQueryFinishedEvent once the
    // result is in, which calls Core's OnPlayerSpawnPadQueryFinished native. Where that native couldn't be added
    // (HookVTables), the game mode looks for the query's end on a timer, SpawnPadQueryCheckInterval seconds apart, for up
    // to twenty seconds.
    constexpr float SpawnPadQueryCheckInterval = 0.1f;
    constexpr int32_t SpawnPadQueryChecks = 200;
    constexpr float BuildingCellSizeXY = 512.0f;
    constexpr float BuildingCellSizeZ = 384.0f;
    constexpr float UnitNavBoundsExtent = 1.0f;

    bool HasPlacementTag(const FGameplayTagContainer& Container, FName Tag)
    {
        const auto Matches = [Tag](const FGameplayTag& Owned) { return Owned.TagName == Tag; };
        return std::any_of(Container.GameplayTags.begin(), Container.GameplayTags.end(), Matches) || std::any_of(Container.ParentTags.begin(), Container.ParentTags.end(), Matches);
    }

    double NowMs()
    {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // The fallback without Core's native: the query has ended once its wrapper has left its manager's running wrappers,
    // or, where the build has no such list, once it has a result.
    void CheckQueryEnded(FWeakObjectPtr GameMode, FWeakObjectPtr WorldManager, FWeakObjectPtr Query, int32_t ChecksLeft, bool bSeenRunning)
    {
        AFortGameModeZone* Zone = static_cast<AFortGameModeZone*>(GameMode.Get());
        UWorld* World = Zone ? Zone->GetWorld() : nullptr;
        if (!World)
        {
            return;
        }

        UEnvQueryInstanceBlueprintWrapper* PlacementQuery = static_cast<UEnvQueryInstanceBlueprintWrapper*>(Query.Get());
        const std::optional<bool> bRunning = UEnvQueryManager::IsQueryRunning(PlacementQuery);
        bSeenRunning = bSeenRunning || bRunning.value_or(false);
        const bool bEnded = !PlacementQuery || (bSeenRunning && !bRunning.value_or(true)) || PlacementQuery->GetResultsAsActors().Num() > 0;
        if (!bEnded && ChecksLeft > 0)
        {
            FTimerHandle QueryTimerHandle;
            World->GetTimerManager().SetTimer(QueryTimerHandle, FTimerDelegate::CreateLambda([GameMode, WorldManager, Query, ChecksLeft, bSeenRunning]()
            {
                CheckQueryEnded(GameMode, WorldManager, Query, ChecksLeft - 1, bSeenRunning);
            }), SpawnPadQueryCheckInterval, false);
            return;
        }

        Zone->OnPlayerSpawnPadQueryFinished(static_cast<AFortWorldManager*>(WorldManager.Get()), PlacementQuery);
    }

    // Until the placement query is done: its OnQueryFinishedEvent calls the game mode's OnPlayerSpawnPadQueryFinished,
    // bound before UEnvQueryManager first ticks the query. A query RunEQSQuery couldn't start (FEnvQueryRequest::Execute
    // gave it no ID, so its wrapper never joined its manager's running wrappers) never broadcasts: it is done now, with
    // no result.
    void WaitForQuery(AFortGameModeZone* Zone, AFortWorldManager* WorldManager, UEnvQueryInstanceBlueprintWrapper* Query)
    {
        UFunction* QueryFinished = AFortGameModeZone::GetOnPlayerSpawnPadQueryFinishedFunction();
        if (!QueryFinished || !UEnvQueryInstanceBlueprintWrapper::HasOnQueryFinishedEvent())
        {
            CheckQueryEnded(FWeakObjectPtr(Zone), FWeakObjectPtr(WorldManager), FWeakObjectPtr(Query), SpawnPadQueryChecks, false);
            return;
        }

        if (UEnvQueryManager::IsQueryRunning(Query) == std::optional<bool>(false))
        {
            Zone->OnPlayerSpawnPadQueryFinished(WorldManager, Query);
            return;
        }
        FMulticastScriptDelegate& OnQueryFinishedEvent = Query->OnQueryFinishedEvent;
        OnQueryFinishedEvent.AddUniqueDynamic(Zone, QueryFinished->GetFName());
    }
}

void AFortGameModeZone::FinishWorldInitialization(AFortWorldManager* WorldManager)
{
#if WITH_SERVER_CODE
    // Battle Royale has no spawn pad, missions or husks: only AFortGameMode's part (bWorldIsReady, which 1.7.2's
    // AFortGameSessionDedicated waits for before it creates an Athena session, 0x6C65E5). Its own override (MapInfo, the
    // warmup starts) runs after this one (SubclassFinishWorldInitializationHK).
    if (AFortGameModeAthena* Athena = Cast<AFortGameModeAthena>(reinterpret_cast<UObject*>(this)))
    {
        AFortGameMode::FinishWorldInitialization(WorldManager);
        Athena->ApplyContainerSpawnPercents();
        Athena->SpawnVendingMachines();
        Athena->SpawnBGAConsumables();
        // 3.0's POI volumes: their brush is stripped from the client paks (FortPoiVolume.cpp).
        AFortPoiVolume::RestorePoiVolumes(GetWorld());
        AFortPoiManager::WatchInitialization(GetWorld());
        ABuildingContainer::LogWorldContainers(GetWorld());
        Athena->StartMatchResults();
        Athena->StartWarmupReports();
        Athena->AskEventCalendarRevision();
        // The llamas, the spawn percents, the warmup's reports and the results from the game state's GamePhaseChanged,
        // the match's playlist from its OnCurrentPlaylistLoaded.
        Athena->BindMatchEvents();
        FCoreAthenaBotManager::Create(Athena);
        return;
    }

    PlacePlayerSpawnPad(WorldManager);
    if (WorldManager)
    {
        WorldManager->CreateMissionManager();
    }
    AFortGameMode::FinishWorldInitialization(WorldManager);

    UWorld* World = GetWorld();
    ABuildingContainer::LogWorldContainers(World);
    UNavigationSystem* NavSys = World ? World->GetNavigationSystem() : nullptr;
    if (NavSys)
    {
        // The supported agents' navigation data: FortNavMesh-Husk and FortNavMesh-Smasher, the husks' navmeshes (their
        // agents' navigation data and the system's main one), and the graph. Up to 4.5 the navigation system spawns them
        // by itself once the zone's unit NavMeshBoundsVolume is in the world (bAutoCreateNavigationData). From 5.0 (UE
        // 4.21) it doesn't: the world settings' UNavigationSystemModuleConfig takes the navigation system class default's
        // flags only when its class is UNavigationSystemV1 itself (PostInitProperties, 6.00 CL-4395664 0x2BF82A0), so a
        // FortNavSystem config keeps bAutoSpawnMissingNavData false and the system is made with bAutoCreateNavigationData
        // off (CreateAndConfigureNavigationSystem, 0x2BCFF10). A zone whose map saves its navmeshes still gets those
        // registered at world init. A generated zone had none (5.30's Zone_Temperate_Suburban logs "Unable to find
        // RecastNavMesh instance" where 4.4's has a FortNavMesh-Husk): the husks' agent had no navmesh, the system had no
        // main navigation data, and every hunt ended without a path (#408).
        //
        // Epic's server spawns them in UFortNavSystem::PrepareBoundsAndUnlock (6.00 0xAF37E0, 4.5 0x93E760), the virtual
        // its stripped world-ready code gives the grid's bounds to: with no navigation data yet,
        // SpawnMissingNavigationData then ProcessRegistrationCandidates; with no graph, SpawnMissingNavigationData; then
        // the octree lock, ConditionalPopulateNavOctree, the graph's OnWorldInitialized and ReleaseInitialBuildingLock.
        // Core's graph is spawned earlier (when the support grid is laid out), so neither test would pass here: Core asks
        // for whatever is missing. SpawnMissingNavigationData spawns only the agents with no data, so an older build's
        // navmeshes, a map's saved ones and the graph stay as they are. It comes before the octree, as in Epic's:
        // ConditionalPopulateNavOctree makes one only when a registered navigation data supports runtime generation (the
        // navmeshes do, the graph doesn't), and the navmeshes build their tiles from it.
        if (UNavigationSystem::CanSpawnMissingNavigationData())
        {
            const TArray<ANavigationData*>& NavDataSet = NavSys->NavDataSet;
            const int32_t NumNavData = NavDataSet.Num();
            NavSys->SpawnMissingNavigationData();
            NavSys->ProcessRegistrationCandidates();
            std::wstring Names;
            for (ANavigationData* NavData : NavDataSet)
            {
                Names += (Names.empty() ? L"" : L", ") + (NavData ? reinterpret_cast<UObject*>(NavData)->GetName().ToStdString() : std::wstring(L"none"));
            }
            ANavigationData* MainNavData = NavSys->GetDefaultNavDataInstance();
            UE_LOG(LogNavigation, Log, TEXT("Navigation data for %d agents: %s (%d spawned); main %s"), NavSys->SupportedAgents.Num(), Names.c_str(), NavDataSet.Num() - NumNavData,
                MainNavData ? *reinterpret_cast<UObject*>(MainNavData)->GetName() : TEXT("none"));
        }

        NavSys->SetNavigationOctreeLock(false);
        NavSys->ConditionalPopulateNavOctree();

        AFortGameStateZone* GameState = World ? Cast<AFortGameStateZone>(reinterpret_cast<UObject*>(World->GetGameState())) : nullptr;
        UBuildingStructuralSupportSystem* SupportSystem = GameState ? static_cast<UBuildingStructuralSupportSystem*>(GameState->StructuralSupportSystem) : nullptr;
        FVector GridMin(0, 0, 0);
        int32_t GridSize[3] = {};
        const bool bHasGrid = SupportSystem && SupportSystem->GetGrid(GridMin, GridSize);
        const FVector GridMax = GridMin + FVector(GridSize[0] * BuildingCellSizeXY, GridSize[1] * BuildingCellSizeXY, GridSize[2] * BuildingCellSizeZ);

        bool bHasWorldBounds = false;
        FVector WorldMin(0, 0, 0);
        FVector WorldMax(0, 0, 0);
        int32_t NumBuildings = 0;
        int32_t NumOutsideGrid = 0;
        float FarthestOutside = 0.0f;
        ABuildingActor* FarthestBuilding = nullptr;
        FVector FarthestOrigin(0, 0, 0);
        FVector FarthestExtent(0, 0, 0);
        for (TObjectIterator<ABuildingActor> It; It; ++It)
        {
            ABuildingActor* Building = *It;
            if (Building->GetWorld() != World || Building->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
            {
                continue;
            }
            FVector Origin;
            FVector Extent;
            Building->GetActorBounds(true, Origin, Extent);
            if (Extent.X <= 0.0f && Extent.Y <= 0.0f && Extent.Z <= 0.0f)
            {
                continue;
            }
            FVector Min = Origin - Extent;
            FVector Max = Origin + Extent;
            if (bHasGrid)
            {
                const float Outside = std::max({ GridMin.X - Min.X, GridMin.Y - Min.Y, GridMin.Z - Min.Z, Max.X - GridMax.X, Max.Y - GridMax.Y, Max.Z - GridMax.Z });
                if (Outside > 0.0f)
                {
                    ++NumOutsideGrid;
                    if (Outside > FarthestOutside)
                    {
                        FarthestOutside = Outside;
                        FarthestBuilding = Building;
                        FarthestOrigin = Origin;
                        FarthestExtent = Extent;
                    }
                }
                Min = FVector(std::max(Min.X, GridMin.X), std::max(Min.Y, GridMin.Y), std::max(Min.Z, GridMin.Z));
                Max = FVector(std::min(Max.X, GridMax.X), std::min(Max.Y, GridMax.Y), std::min(Max.Z, GridMax.Z));
                if (Min.X > Max.X || Min.Y > Max.Y || Min.Z > Max.Z)
                {
                    continue;
                }
            }
            WorldMin = bHasWorldBounds ? FVector(std::min(WorldMin.X, Min.X), std::min(WorldMin.Y, Min.Y), std::min(WorldMin.Z, Min.Z)) : Min;
            WorldMax = bHasWorldBounds ? FVector(std::max(WorldMax.X, Max.X), std::max(WorldMax.Y, Max.Y), std::max(WorldMax.Z, Max.Z)) : Max;
            bHasWorldBounds = true;
            ++NumBuildings;
        }

        if (FarthestBuilding)
        {
            UE_LOG(LogNavigation, Warning, TEXT("%d building actors extend past the building grid (%.0f, %.0f, %.0f) - (%.0f, %.0f, %.0f); farthest %s (%s) at (%.0f, %.0f, %.0f) extent (%.0f, %.0f, %.0f), clamped to the grid"),
                NumOutsideGrid, GridMin.X, GridMin.Y, GridMin.Z, GridMax.X, GridMax.Y, GridMax.Z, *FarthestBuilding->GetName(), *FarthestBuilding->GetClass()->GetName(),
                FarthestOrigin.X, FarthestOrigin.Y, FarthestOrigin.Z, FarthestExtent.X, FarthestExtent.Y, FarthestExtent.Z);
        }
        if (bHasWorldBounds)
        {
            UE_LOG(LogNavigation, Log, TEXT("Navigation bounds (%.0f, %.0f, %.0f) - (%.0f, %.0f, %.0f) from %d building actors"), WorldMin.X, WorldMin.Y, WorldMin.Z, WorldMax.X, WorldMax.Y, WorldMax.Z, NumBuildings);
        }

        int32_t NumBoundsVolumes = 0;
        for (TObjectIterator<ANavMeshBoundsVolume> It; It && bHasWorldBounds; ++It)
        {
            ANavMeshBoundsVolume* NavBounds = *It;
            if (NavBounds->GetWorld() != World || NavBounds->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
            {
                continue;
            }
            FVector Origin;
            FVector Extent;
            NavBounds->GetActorBounds(false, Origin, Extent);
            if (Extent.X > UnitNavBoundsExtent || Extent.Y > UnitNavBoundsExtent || Extent.Z > UnitNavBoundsExtent)
            {
                UE_LOG(LogNavigation, Log, TEXT("Keeping %s's own navigation bounds (%.0f, %.0f, %.0f) - (%.0f, %.0f, %.0f)"), *NavBounds->GetFullName(),
                    Origin.X - Extent.X, Origin.Y - Extent.Y, Origin.Z - Extent.Z, Origin.X + Extent.X, Origin.Y + Extent.Y, Origin.Z + Extent.Z);
                ++NumBoundsVolumes;
                continue;
            }
            if (USceneComponent* Root = NavBounds->GetRootComponent())
            {
                Root->Mobility = EComponentMobility::Movable;
            }
            NavBounds->SetActorScale3D(WorldMax - WorldMin);
            NavBounds->SetActorLocation((WorldMin + WorldMax) * 0.5f);
            NavSys->OnNavigationBoundsUpdated(NavBounds);
            ++NumBoundsVolumes;
        }
        if (bHasWorldBounds && NumBoundsVolumes == 0)
        {
            // The navmeshes build inside the navigation bounds only. A zone map streams /Game/Maps/NavMeshBounds for its
            // unit volume (Zone_Temperate_Suburban and Zone_Onboarding_Suburban_a prestream it on 5.30 and 6.00), which is
            // what gets the zone's size above; a zone without one shows here.
            UE_LOG(LogNavigation, Warning, TEXT("No NavMeshBoundsVolume in the world: the navmeshes have no bounds to build in"));
        }

        const double Started = NowMs();
        NavSys->ReleaseInitialBuildingLock();
        UE_LOG(LogNavigation, Log, TEXT("Navmesh RebuildAll time:%.3fms"), NowMs() - Started);
    }

    if (AFortNavigationGraph* NavGraph = AFortNavigationGraph::GetCurrent(World))
    {
        NavGraph->OnWorldInitialized();
    }

    if (AFortGameModeOutpost* Outpost = Cast<AFortGameModeOutpost>(reinterpret_cast<UObject*>(this)))
    {
        Outpost->KeepStructureLimit();
    }
    // Challenge the Horde and Build the Fort (5.0): their game mode's own FinishWorldInitialization runs next.
    AFortDeployableBaseManager::WatchHorde(this);
#endif
}

UClass* AFortGameModeZone::DetermineAIDirectorClass() const
{
    UFortGameData* GameData = UFortAssetManager::Get().GetGameData();
    UClass* AIDirectorClass = nullptr;
    if (GameData)
    {
        const TArray<TAssetSubclassOf<AFortAIDirector>>& AIDirectors = GameData->AIDirectors;
        const int32_t Index = OverrideAIDirectorIndex;
        AIDirectorClass = Index >= 0 && Index < AIDirectors.Num() ? SoftElementAt(AIDirectors, Index).LoadSynchronous() : nullptr;
    }
    return AIDirectorClass && AIDirectorClass->IsChildOf(AFortAIDirector::StaticClass()) ? AIDirectorClass : AFortAIDirector::StaticClass();
}

void AFortGameModeZone::CreateAIDirector()
{
#if WITH_SERVER_CODE
    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    // Every build's DefaultGameData names /Game/AIDirector/AIDirector_Fortnite, so the native class means it didn't load:
    // 1.10's paks don't ship it. The native director then takes the Blueprint's settings before it finishes spawning.
    UClass* const AIDirectorClass = DetermineAIDirectorClass();
    if (AIDirectorClass == AFortAIDirector::StaticClass())
    {
        // Battle Royale's, for a playlist's AI (6.20: AFortGameModeAthena's constructor sets OverrideAIDirectorIndex 2,
        // DefaultGameData's /Game/Athena/Deimos/AIDirector/Deimos_AIDirector), is a server asset too: the CL's
        // Core-ServerAssets pak has it.
        AFortGameModeAthena* Athena = Cast<AFortGameModeAthena>(reinterpret_cast<UObject*>(this));
        if (Athena && AFortGameModeAthena::HasAISettings() && static_cast<UObject*>(Athena->AISettings))
        {
            UFortGameData* GameData = UFortAssetManager::Get().GetGameData();
            const TArray<TAssetSubclassOf<AFortAIDirector>>* AIDirectors = GameData ? &static_cast<const TArray<TAssetSubclassOf<AFortAIDirector>>&>(GameData->AIDirectors) : nullptr;
            const int32_t Index = OverrideAIDirectorIndex;
            const std::wstring Wanted = AIDirectors && Index >= 0 && Index < AIDirectors->Num() ? SoftElementAt(*AIDirectors, Index).ToStringReference().ToString().ToStdString() : std::wstring(L"none");
            UE_LOG(LogFort, Warning, TEXT("AIDirector: Battle Royale's AI director %s (AIDirectors[%d]) didn't load, so Core's stand-in with Save the World's settings is used (the build's server assets pak has the real one)"),
                Wanted.c_str(), Index);
        }
        const FTransform SpawnTransform;
        AFortAIDirector* NewAIDirector = Cast<AFortAIDirector>(UGameplayStatics::BeginDeferredActorSpawnFromClass(this, AIDirectorClass, SpawnTransform));
        if (NewAIDirector)
        {
            NewAIDirector->ApplyAIDirectorFortniteDefaults();
            NewAIDirector = Cast<AFortAIDirector>(UGameplayStatics::FinishSpawningActor(NewAIDirector, SpawnTransform));
        }
        AIDirector = NewAIDirector;
        return;
    }
    // The real one: the build ships it (to 1.9.1), or a server assets pak (Core-ServerAssets) put it back.
    UE_LOG(LogFort, Log, TEXT("AIDirector: %s loaded, Core's stand-in isn't used"), *AIDirectorClass->GetPathName());
    AIDirector = World->SpawnActor<AFortAIDirector>(AIDirectorClass, FActorSpawnParameters());
#endif
}

void AFortGameModeZone::PostInitializeComponents()
{
    PostInitializeComponentsOG(this);

#if WITH_SERVER_CODE
    UWorld* World = GetWorld();
    AFortGameState* FortGameState = World ? Cast<AFortGameState>(reinterpret_cast<UObject*>(World->GetGameState())) : nullptr;
    const TSubclassOf<AFortVisibilityManager>& ManagerClass = VisibilityManagerClass;
    if (FortGameState && ManagerClass.Get() && !FortGameState->VisibilityManager)
    {
        FortGameState->VisibilityManager = World->SpawnActor<AFortVisibilityManager>(ManagerClass.Get(), FActorSpawnParameters());
    }
#endif
}

APlayerController* AFortGameModeZone::Login(UPlayer* NewPlayer, ENetRole InRemoteRole, const FString& Portal, const FString& Options, const FUniqueNetIdRepl& UniqueId, FString& ErrorMessage)
{
    APlayerController* NewPlayerController = LoginOG(this, NewPlayer, InRemoteRole, Portal, Options, UniqueId, ErrorMessage);

#if WITH_SERVER_CODE
    AFortPlayerController* FortPlayerController = Cast<AFortPlayerController>(reinterpret_cast<UObject*>(NewPlayerController));
    AFortPlayerState* PlayerState = FortPlayerController ? Cast<AFortPlayerState>(reinterpret_cast<UObject*>(static_cast<APlayerState*>(FortPlayerController->PlayerState))) : nullptr;
    UWorld* World = GetWorld();
    AFortGameState* GameState = World ? Cast<AFortGameState>(reinterpret_cast<UObject*>(World->GetGameState())) : nullptr;
    AFortWorldManager* WorldManager = GameState ? static_cast<AFortWorldManager*>(GameState->WorldManager) : nullptr;
    // The player's index in the world record's SavedPlayers. Battle Royale has one too: with no world id to query, the
    // world manager makes a fresh record (6.21 0xFD2A80, then WorldQueryComplete). The id is whose building a piece is:
    // InitializeBuildingActor gives it to the piece (OwnerPersistentID, and bPlayerPlaced), and
    // ABuildingActor::GetOwningPlayerState (6.21 0xD24B20) finds the player state with it, which is how the Playground
    // building challenges' guides (AthenaLayoutBuildRequirement's StartProgress) credit the builder (CoreV2 #443).
    if (PlayerState && WorldManager)
    {
        const FString PlayerName = PlayerState->GetPlayerName();
        PlayerState->WorldPlayerId = WorldManager->GetOrAllocatePlayerID(UniqueId.ToString(), PlayerName);
        UE_LOG(LogFort, Log, TEXT("Login: %s is world player %d"), *PlayerName, static_cast<int32_t>(PlayerState->WorldPlayerId));
    }

    // The escape menu's team (UFortPartyContext::GetLocalTeamTeamMemberInfo, UFortUITeamInfo::GetTeamMembers) lists the
    // players by party, and a player state without a PartyOwnerUniqueId is in none: the menu showed an empty slot, with no
    // name, banner, power or stats (CoreV2 #111; a 1.7.2 client listed nobody until its player state had one). PostLogin
    // (1.7.2 0x574580) asks the client for it (ClientRegisterWithParty, 0x7F9A30), which answers ServerSetPartyOwner only
    // when its persistent party has a leader ("Failed to get persistent party leader", 0x80BDE0). A player left without
    // one gets the party owner the client gave the lobby beacon, else leads its own party. Battle Royale keeps the game's.
    if (FortPlayerController && PlayerState && !Cast<AFortGameModeAthena>(reinterpret_cast<UObject*>(this)))
    {
        const FUniqueNetIdRepl& PartyOwnerUniqueId = PlayerState->PartyOwnerUniqueId;
        if (!PartyOwnerUniqueId.IsValid() && UniqueId.IsValid())
        {
            const FUniqueNetIdRepl* PartyOwner = &UniqueId;
            for (TObjectIterator<ALobbyBeaconPlayerState> It; It; ++It)
            {
                const FUniqueNetIdRepl& BeaconId = It->UniqueId;
                const FUniqueNetIdRepl& BeaconPartyOwner = It->PartyOwnerUniqueId;
                if (It->GetWorld() == World && BeaconPartyOwner.IsValid() && BeaconId.ToString().ToStdString() == UniqueId.ToString().ToStdString())
                {
                    PartyOwner = &BeaconPartyOwner;
                    break;
                }
            }
            FortPlayerController->ServerSetPartyOwner(*PartyOwner);
            const FUniqueNetIdRepl& NewPartyOwner = PlayerState->PartyOwnerUniqueId;
            UE_LOG(LogFort, Log, TEXT("Login: %s is in the party of %s"), *UniqueId.ToString(), *NewPartyOwner.ToString());
        }
    }

    AFortPlayerStateOutpost* OutpostPlayerState = Cast<AFortPlayerStateOutpost>(reinterpret_cast<UObject*>(PlayerState));
    if (OutpostPlayerState && Cast<AFortGameModeOutpost>(reinterpret_cast<UObject*>(this)) && (Natives::AFortPlayerStateOutpost_bIsOutpostOwner > 0 || Finder::ResolveOutpostOwnerLive()))
    {
        bool bHasOwner = false;
        for (TObjectIterator<AFortPlayerStateOutpost> It; It && !bHasOwner; ++It)
        {
            bHasOwner = *It != OutpostPlayerState && It->GetWorld() == World && It->IsOutpostOwner();
        }
        if (!bHasOwner)
        {
            OutpostPlayerState->bIsOutpostOwner = true;
            OutpostPlayerState->bCanEditOutpost = true;
        }
    }
#endif
    return NewPlayerController;
}

FFortPlayerSpawnPadPlacementData* AFortGameModeZone::GetPlayerSpawnPadPlacementData()
{
    UWorld* World = GetWorld();
    AFortGameStateZone* GameState = World ? Cast<AFortGameStateZone>(reinterpret_cast<UObject*>(World->GetGameState())) : nullptr;
    UFortMissionGenerator* MissionGenerator = GameState ? GameState->GetMissionGenerator() : nullptr;
    if (MissionGenerator && MissionGenerator->bUseOverridePlayerSpawnPadPlacementData)
    {
        return &static_cast<FFortPlayerSpawnPadPlacementData&>(MissionGenerator->OverridePlayerSpawnPadPlacementData);
    }
    UFortGameData* GameData = UFortAssetManager::Get().GetGameData();
    return GameData ? &static_cast<FFortPlayerSpawnPadPlacementData&>(GameData->DefaultPlayerSpawnPadPlacementData) : nullptr;
}

void AFortGameModeZone::PlacePlayerSpawnPad(AFortWorldManager* WorldManager)
{
#if WITH_SERVER_CODE
    FFortPlayerSpawnPadPlacementData* PlacementData = GetPlayerSpawnPadPlacementData();
    UEnvQuery* PlacementQuery = PlacementData ? static_cast<UEnvQuery*>(PlacementData->PlacementQuery) : nullptr;
    UEnvQueryInstanceBlueprintWrapper* Query = PlacementQuery ? UEnvQueryManager::RunEQSQuery(this, PlacementQuery, this, EEnvQueryRunMode::SingleResult, nullptr) : nullptr;
    if (!Query)
    {
        UE_LOG(LogFortPlacement, Error, TEXT("PlacePlayerSpawnPad: No valid placement actor could be found!  Failed to spawn player spawn pad."));
        return;
    }

    WaitForQuery(this, WorldManager, Query);
#endif
}

void AFortGameModeZone::OnPlayerSpawnPadQueryFinished(AFortWorldManager* WorldManager, UEnvQueryInstanceBlueprintWrapper* PlacementQuery)
{
#if WITH_SERVER_CODE
    const TArray<AActor*> Results = PlacementQuery ? PlacementQuery->GetResultsAsActors() : TArray<AActor*>();
    ABuildingActor* PlacementActor = Results.Num() > 0 ? Cast<ABuildingActor>(Results[0]) : nullptr;
    FFortPlayerSpawnPadPlacementData* PlacementData = GetPlayerSpawnPadPlacementData();
    if (!PlacementActor || !WorldManager || !PlacementData)
    {
        UE_LOG(LogFortPlacement, Error, TEXT("PlacePlayerSpawnPad: No valid placement actor could be found!  Failed to spawn player spawn pad."));
        return;
    }

    UE_LOG(LogFortPlacement, Log, TEXT("PlacePlayerSpawnPad: Found spot %s with initial tags"), *PlacementActor->GetName());

    if (AActor* SpawnPad = WorldManager->SpawnPlayerSpawnPad(*PlacementData, PlacementActor->GetTransform()))
    {
        FGameplayTagContainer& PlacementTags = PlacementActor->StaticGameplayTags;
        UBlueprintGameplayTagLibrary::AppendGameplayTagContainers(PlacementTags, PlacementData->TagsToAddToChosenPlacementActor);
        ActiveSpawnPad = SpawnPad;
    }
#endif
}

DEFINE_FUNCTION(AFortGameModeZone::execOnPlayerSpawnPadQueryFinished)
{
    P_GET_OBJECT(UEnvQueryInstanceBlueprintWrapper, Z_Param_QueryInstance);
    P_GET_PROPERTY(UByteProperty, Z_Param_QueryStatus);
    P_FINISH;
    P_NATIVE_BEGIN;
    // Any status but Success leaves the query without a result, which OnPlayerSpawnPadQueryFinished reports. The world
    // manager is the game state's, the one that finished the world's initialization.
    AFortGameModeZone* Zone = static_cast<AFortGameModeZone*>(Context);
    UWorld* World = Zone->GetWorld();
    AFortGameState* GameState = World ? Cast<AFortGameState>(reinterpret_cast<UObject*>(World->GetGameState())) : nullptr;
    AFortWorldManager* WorldManager = GameState ? static_cast<AFortWorldManager*>(GameState->WorldManager) : nullptr;
    Zone->OnPlayerSpawnPadQueryFinished(WorldManager, Z_Param_QueryInstance);
    P_NATIVE_END;
}

void AFortGameModeZone::OnZoneEnd()
{
    if (Natives::AFortGameModeZone_OnZoneEndIndex >= 0)
    {
        CallVirtual<void>(this, Natives::AFortGameModeZone_OnZoneEndIndex);
    }
}

ptrdiff_t AFortGameModeZone::EndOfZoneRemainTimeOffset()
{
    return Member_EndOfZoneRemainTime.GetOffsetChecked();
}

ptrdiff_t AFortGameModeZone::ActiveSpawnPadOffset()
{
    return Member_ActiveSpawnPad.GetOffsetChecked();
}

void AFortGameModeZone::Hook()
{
    Hooks::Detour(L"AFortGameModeZone::SpawnDefaultPawnFor_Implementation", reinterpret_cast<void*>(Natives::AFortGameModeZone_SpawnDefaultPawnFor), &SpawnDefaultPawnFor_ImplementationHK, nullptr);

    for (uint8_t* Call : Sites::ChoosePlayerStartGoalManagerCalls)
    {
        Hooks::RedirectCall(L"AFortGameModeZone::ChoosePlayerStart AFortAIGoalManager::GetCurrent", Call, reinterpret_cast<void*>(&AFortAIGoalManager::GetCurrent));
    }

    for (uint8_t* Call : Sites::AIDirectorGetCurrentCalls)
    {
        Hooks::RedirectCall(L"AFortAIDirector::GetCurrent", Call, reinterpret_cast<void*>(&AFortAIDirector::GetCurrent));
    }

    Finder::FPlayerSpawnQuerySites SpawnSites;
    if (Finder::ResolvePlayerSpawnQueriesLive(SpawnSites))
    {
        Natives::AFortGameMode_PickPlayerSpawnActor = reinterpret_cast<decltype(Natives::AFortGameMode_PickPlayerSpawnActor)>(SpawnSites.PickSpawnActor);
        Natives::AFortGameMode_GetFriendlyPlayerPawnsIndex = SpawnSites.FriendlyPawnsIndex;
        for (uint8_t* Call : SpawnSites.FriendlyCalls)
        {
            Hooks::RedirectCall(L"AFortGameModeZone::ChoosePlayerStart friendly player spawn query", Call, reinterpret_cast<void*>(&PickFriendlyPlayerSpawnActorCall));
        }
        Hooks::RedirectCall(L"AFortGameModeZone::ChoosePlayerStart initial spawn query", SpawnSites.InitialCall, reinterpret_cast<void*>(&PickInitialPlayerSpawnActorCall));
        Hooks::RedirectCall(L"AFortGameModeZone::ChoosePlayerStart backup spawn query", SpawnSites.BackupCall, reinterpret_cast<void*>(&PickBackupPlayerSpawnActorCall));
    }
}

void AFortGameModeZone::GatherPlayerSpawnActors(AController* Player, bool bInitialTags, bool bNearFriendlyPlayers, TArray<AActor*>& OutCandidates)
{
#if WITH_SERVER_CODE
    OutCandidates.Reset();
    UWorld* World = GetWorld();
    AFortGameStateZone* GameState = World ? Cast<AFortGameStateZone>(reinterpret_cast<UObject*>(World->GetGameState())) : nullptr;
    if (!GameState)
    {
        return;
    }
    AFortWorldManager* WorldManager = static_cast<AFortWorldManager*>(GameState->WorldManager);
    UFortZoneTheme* ZoneTheme = static_cast<UFortZoneTheme*>(GameState->ZoneTheme);

    std::vector<FName> RequiredTags;
    const FGameplayTagContainer* SourceTags = ZoneTheme ? &(bInitialTags ? ZoneTheme->InitialPlayerSpawnTags : ZoneTheme->BackupPlayerSpawnTags) : nullptr;
    const FGameplayTagContainer* OverrideTags = WorldManager ? &(bInitialTags ? WorldManager->OverrideInitialPlayerSpawnTags : WorldManager->OverrideBackupPlayerSpawnTags) : nullptr;
    if (OverrideTags && OverrideTags->Num() > 0)
    {
        SourceTags = OverrideTags;
    }
    if (SourceTags)
    {
        for (const FGameplayTag& Tag : SourceTags->GameplayTags)
        {
            RequiredTags.push_back(Tag.TagName);
        }
    }
    else if (bInitialTags)
    {
        RequiredTags.push_back(FName(L"PlacementActor.PlayerSpawn"));
    }
    else
    {
        RequiredTags.push_back(FName(L"Activities.Placement.OnGround"));
        RequiredTags.push_back(FName(L"Activities.Placement.Outside"));
    }

    std::vector<FVector> FriendlyLocations;
    if (bNearFriendlyPlayers && Natives::AFortGameMode_GetFriendlyPlayerPawnsIndex >= 0)
    {
        TArray<AActor*> FriendlyPawns;
        CallVirtual<void>(this, Natives::AFortGameMode_GetFriendlyPlayerPawnsIndex, Player, &FriendlyPawns);
        for (AActor* Pawn : FriendlyPawns)
        {
            if (Pawn)
            {
                FriendlyLocations.push_back(Pawn->GetTransform().GetLocation());
            }
        }
    }
    const FVector FriendlyExtent = WorldManager ? static_cast<FVector>(WorldManager->FriendlyPlayerSpawnExtent) : FVector(0, 0, 0);
    if (bNearFriendlyPlayers && FriendlyLocations.empty())
    {
        return;
    }

    for (TObjectIterator<AFortPlacementActor> It; It; ++It)
    {
        AFortPlacementActor* PlacementActor = *It;
        if (PlacementActor->GetWorld() != World || PlacementActor->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
        {
            continue;
        }
        const FGameplayTagContainer& StaticTags = PlacementActor->StaticGameplayTags;
        const bool bHasDefaultTags = PlacementActor->HasDefaultTags();
        if (!std::all_of(RequiredTags.begin(), RequiredTags.end(), [&](FName Tag) { return HasPlacementTag(StaticTags, Tag) || (bHasDefaultTags && HasPlacementTag(PlacementActor->DefaultTags, Tag)); }))
        {
            continue;
        }
        if (bNearFriendlyPlayers)
        {
            const FVector Location = PlacementActor->GetTransform().GetLocation();
            const bool bNearFriendly = std::any_of(FriendlyLocations.begin(), FriendlyLocations.end(), [&](const FVector& Friendly)
            {
                return std::abs(Location.X - Friendly.X) <= FriendlyExtent.X && std::abs(Location.Y - Friendly.Y) <= FriendlyExtent.Y && std::abs(Location.Z - Friendly.Z) <= FriendlyExtent.Z;
            });
            if (!bNearFriendly)
            {
                continue;
            }
        }
        OutCandidates.Add(PlacementActor);
    }

    UE_LOG(LogFortPlacement, Log, TEXT("ChoosePlayerStart: %d %s spawn candidates for %s"), OutCandidates.Num(), bNearFriendlyPlayers ? TEXT("friendly player") : bInitialTags ? TEXT("initial") : TEXT("backup"), Player ? *Player->GetName() : TEXT("None"));
#endif
}

AActor* AFortGameModeZone::PickFriendlyPlayerSpawnActorCall(AFortGameModeZone* This, AController* Player, UClass* PawnClass, TArray<AActor*>* Candidates, float DangerExclusionRadius, float ObjectiveExclusionRadius)
{
    if (This && Candidates)
    {
        This->GatherPlayerSpawnActors(Player, false, true, *Candidates);
    }
    return Natives::AFortGameMode_PickPlayerSpawnActor(This, Player, PawnClass, Candidates, DangerExclusionRadius, ObjectiveExclusionRadius);
}

AActor* AFortGameModeZone::PickInitialPlayerSpawnActorCall(AFortGameModeZone* This, AController* Player, UClass* PawnClass, TArray<AActor*>* Candidates, float DangerExclusionRadius, float ObjectiveExclusionRadius)
{
    if (This && Candidates)
    {
        This->GatherPlayerSpawnActors(Player, true, false, *Candidates);
    }
    return Natives::AFortGameMode_PickPlayerSpawnActor(This, Player, PawnClass, Candidates, DangerExclusionRadius, ObjectiveExclusionRadius);
}

AActor* AFortGameModeZone::PickBackupPlayerSpawnActorCall(AFortGameModeZone* This, AController* Player, UClass* PawnClass, TArray<AActor*>* Candidates, float DangerExclusionRadius, float ObjectiveExclusionRadius)
{
    if (!This || !Candidates)
    {
        return Natives::AFortGameMode_PickPlayerSpawnActor(This, Player, PawnClass, Candidates, DangerExclusionRadius, ObjectiveExclusionRadius);
    }
    This->GatherPlayerSpawnActors(Player, false, false, *Candidates);

    // The backup spots are every outside spot on the map and the native picks one at random, so a player whose pad
    // and friendly spot were taken (a friend still standing where they spawned) could land anywhere, on the outpost
    // outside the Storm Shield (#144). The pad and the friends are where the game wants them, so the backup spots are
    // tried nearest to those first, each through the native's own checks; with neither, the random pick stays.
    std::vector<FVector> Anchors;
    AActor* SpawnPad = Member_ActiveSpawnPad.IsAvailable() ? static_cast<AActor*>(This->ActiveSpawnPad) : nullptr;
    if (SpawnPad)
    {
        Anchors.push_back(SpawnPad->GetTransform().GetLocation());
    }
    if (Natives::AFortGameMode_GetFriendlyPlayerPawnsIndex >= 0)
    {
        TArray<AActor*> FriendlyPawns;
        CallVirtual<void>(This, Natives::AFortGameMode_GetFriendlyPlayerPawnsIndex, Player, &FriendlyPawns);
        for (AActor* Pawn : FriendlyPawns)
        {
            if (Pawn)
            {
                Anchors.push_back(Pawn->GetTransform().GetLocation());
            }
        }
    }
    if (Anchors.empty())
    {
        return Natives::AFortGameMode_PickPlayerSpawnActor(This, Player, PawnClass, Candidates, DangerExclusionRadius, ObjectiveExclusionRadius);
    }

    std::vector<std::pair<float, AActor*>> ByDistance;
    for (AActor* Candidate : *Candidates)
    {
        if (!Candidate)
        {
            continue;
        }
        const FVector Location = Candidate->GetTransform().GetLocation();
        float Nearest = FLT_MAX;
        for (const FVector& Anchor : Anchors)
        {
            const float DX = Location.X - Anchor.X;
            const float DY = Location.Y - Anchor.Y;
            const float DZ = Location.Z - Anchor.Z;
            Nearest = std::min(Nearest, DX * DX + DY * DY + DZ * DZ);
        }
        ByDistance.emplace_back(Nearest, Candidate);
    }
    std::sort(ByDistance.begin(), ByDistance.end(), [](const auto& A, const auto& B) { return A.first < B.first; });

    TArray<AActor*> Single;
    for (const auto& [DistanceSquared, Candidate] : ByDistance)
    {
        Single.Reset();
        Single.Add(Candidate);
        if (AActor* Picked = Natives::AFortGameMode_PickPlayerSpawnActor(This, Player, PawnClass, &Single, DangerExclusionRadius, ObjectiveExclusionRadius))
        {
            UE_LOG(LogFortPlacement, Log, TEXT("ChoosePlayerStart: backup spawn %s for %s, %.0f from the spawn pad or a friendly player"), *Picked->GetName(), Player ? *Player->GetName() : TEXT("None"), std::sqrt(DistanceSquared));
            return Picked;
        }
    }
    return nullptr;
}

namespace
{
    using FFinishWorldInitialization = void (*)(AFortGameModeZone* This, AFortWorldManager* WorldManager);
    std::unordered_map<void**, FFinishWorldInitialization> SubclassFinishWorldInitialization;
}

void AFortGameModeZone::HookVTables()
{
    // The spawn pad's placement query's OnQueryFinishedEvent calls this native (FEQSQueryDoneSignature's parameters)
    // on the game mode, through the game mode's class's FuncMap.
    const UFunction* QueryFinishedSignature = Finder::ResolveClassFunctionMap() ? UEnvQueryInstanceBlueprintWrapper::GetQueryFinishedSignature() : nullptr;
    OnPlayerSpawnPadQueryFinishedFunction = QueryFinishedSignature && StaticClass()
        ? StaticClass()->AddNativeFunction(L"OnPlayerSpawnPadQueryFinished", &execOnPlayerSpawnPadQueryFinished, QueryFinishedSignature) : nullptr;

    // What a spawn on the spawn pad's player spots binds the player's home with, as AFortGameMode's spawn does
    // (TrySpawnDefaultPawn). Without it the pad's spawns stay AFortGameMode's.
    if (!Configuration::Get().IsBattleRoyale() && AFortPlayerState::HasHomeActor() && Natives::AFortGameMode_SpawnDefaultPawnFor)
    {
        Finder::ResolveSetHomeActorLive(AFortPlayerState::HomeActorOffset());
    }

    if (!Finder::ResolveGameModeZoneLive())
    {
        return;
    }

    const int32_t FinishIndex = Natives::AFortGameMode_FinishWorldInitializationIndex;
    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Class = *It;
        UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
        void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
        if (VTable && VTable[FinishIndex] == Natives::SharedEmptyFunction)
        {
            Hooks::SwapVTable(L"AFortGameModeZone::FinishWorldInitialization", VTable, FinishIndex, reinterpret_cast<void*>(&FinishWorldInitializationHK));
        }
        else if (VTable && VTable[FinishIndex] != reinterpret_cast<void*>(&FinishWorldInitializationHK) && VTable[FinishIndex] != reinterpret_cast<void*>(&SubclassFinishWorldInitializationHK))
        {
            void* Original = nullptr;
            if (Hooks::SwapVTable(L"AFortGameModeZone subclass FinishWorldInitialization", VTable, FinishIndex, reinterpret_cast<void*>(&SubclassFinishWorldInitializationHK), &Original))
            {
                SubclassFinishWorldInitialization[VTable] = reinterpret_cast<FFinishWorldInitialization>(Original);
                if (Class->GetName().ToStdString().find(L"DeployableBase") != std::wstring::npos)
                {
                    AFortDeployableBaseManager::ResolveWorldInitialization(Original);
                }
            }
        }
    }

    if (Finder::ResolveLoginLive())
    {
        const int32_t LoginIndex = Natives::AGameModeBase_LoginIndex;
        UObject* ZoneDefault = StaticClass()->GetDefaultObject(false);
        void* const ZoneLogin = ZoneDefault ? (*reinterpret_cast<void***>(ZoneDefault))[LoginIndex] : nullptr;
        LoginOG = reinterpret_cast<decltype(LoginOG)>(ZoneLogin);
        // A subclass's own Login reaches this one through Super::Login, a direct call: 6.21's AFortGameModeAthena::Login
        // (slot 0xE2, 0xC826D0) adds the split-screen options and calls AFortGameModeZone::Login (0xEED620) at 0xC82A19,
        // so the vtable swap alone left Battle Royale's players without the WorldPlayerId the stripped half gives them
        // (CoreV2 #443, below). Builds whose subclass Login doesn't call it (1.7.2's Athena Login, 0x3F2450) keep theirs.
        std::vector<uintptr_t> SubclassLogins;
        for (TObjectIterator<UClass> It; ZoneLogin && It; ++It)
        {
            UClass* Class = *It;
            UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
            void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
            if (VTable && VTable[LoginIndex] == ZoneLogin)
            {
                Hooks::SwapVTable(L"AFortGameModeZone::Login", VTable, LoginIndex, reinterpret_cast<void*>(&LoginHK));
            }
            else if (VTable && VTable[LoginIndex] != reinterpret_cast<void*>(&LoginHK)
                && std::find(SubclassLogins.begin(), SubclassLogins.end(), reinterpret_cast<uintptr_t>(VTable[LoginIndex])) == SubclassLogins.end())
            {
                SubclassLogins.push_back(reinterpret_cast<uintptr_t>(VTable[LoginIndex]));
            }
        }
        for (const uintptr_t SubclassLogin : SubclassLogins)
        {
            const uintptr_t SubclassLoginEnd = Finder::FunctionEnd(SubclassLogin);
            if (SubclassLoginEnd <= SubclassLogin || SubclassLoginEnd - SubclassLogin > 0x2000)
            {
                continue;
            }
            for (const auto& [Call, Target] : Finder::DecodedDirectCalls(SubclassLogin))
            {
                if (Target == reinterpret_cast<uintptr_t>(ZoneLogin))
                {
                    Hooks::RedirectCall(L"AFortGameModeZone subclass Login -> Super::Login", reinterpret_cast<uint8_t*>(Call), reinterpret_cast<void*>(&LoginHK));
                }
            }
        }
    }

    if (!Finder::ResolveAIDirectorLive())
    {
        return;
    }

    const int32_t DirectorIndex = Natives::AFortGameModeZone_CreateAIDirectorIndex;
    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Class = *It;
        UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
        void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
        if (VTable && VTable[DirectorIndex] == Natives::SharedEmptyFunction)
        {
            Hooks::SwapVTable(L"AFortGameModeZone::CreateAIDirector", VTable, DirectorIndex, reinterpret_cast<void*>(&CreateAIDirectorHK));
        }
    }
    // From 6.20 (the Fortnitemares Cube Monsters' playlists): Battle Royale's own CreateAIDirector, once the match's AI
    // settings allow one, calls this function as its Super:: (a direct call, which the swap above doesn't reach).
    if (uint8_t* AthenaSuperCall = Finder::AthenaCreateAIDirectorSuperCall(AFortGameModeAthena::AISettingsOffset()))
    {
        Hooks::RedirectCall(L"AFortGameModeAthena::CreateAIDirector -> AFortGameModeZone::CreateAIDirector", AthenaSuperCall, reinterpret_cast<void*>(&CreateAIDirectorHK));
    }

    UObject* ZoneDefault = StaticClass()->GetDefaultObject(false);
    void** ZoneVTable = ZoneDefault ? *reinterpret_cast<void***>(ZoneDefault) : nullptr;
    int32_t PostInitializeIndex = -1;
    for (int32_t Index = 0; ZoneVTable && Index < 0x400 && PostInitializeIndex < 0; ++Index)
    {
        const uintptr_t Function = reinterpret_cast<uintptr_t>(ZoneVTable[Index]);
        if (Function < Finder::TextBegin() || Function >= Finder::TextEnd())
        {
            break;
        }
        const uintptr_t FunctionEnd = Finder::FunctionEnd(Function);
        for (uintptr_t Site = Function; FunctionEnd > Function && FunctionEnd - Function < 0x100 && Site + 6 <= FunctionEnd; ++Site)
        {
            const uint8_t* Bytes = reinterpret_cast<const uint8_t*>(Site);
            if (Bytes[0] == 0xFF && Bytes[1] == 0x90 && *reinterpret_cast<const int32_t*>(Bytes + 2) == DirectorIndex * static_cast<int32_t>(sizeof(void*)))
            {
                PostInitializeIndex = Index;
                break;
            }
        }
    }

    void* const ZonePostInitialize = PostInitializeIndex >= 0 ? ZoneVTable[PostInitializeIndex] : nullptr;
    if (!ZonePostInitialize)
    {
        Log::Error(L"Finder: AFortGameModeZone::PostInitializeComponents slot not found");
        return;
    }
    PostInitializeComponentsOG = reinterpret_cast<decltype(PostInitializeComponentsOG)>(ZonePostInitialize);
    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Class = *It;
        UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
        void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
        if (VTable && VTable[PostInitializeIndex] == ZonePostInitialize)
        {
            Hooks::SwapVTable(L"AFortGameModeZone::PostInitializeComponents", VTable, PostInitializeIndex, reinterpret_cast<void*>(&PostInitializeComponentsHK));
        }
    }
}

void AFortGameModeZone::CreateAIDirectorHK(AFortGameModeZone* This)
{
    This->CreateAIDirector();
}

void AFortGameModeZone::PostInitializeComponentsHK(AFortGameModeZone* This)
{
    This->PostInitializeComponents();
}

APlayerController* AFortGameModeZone::LoginHK(AFortGameModeZone* This, UPlayer* NewPlayer, ENetRole InRemoteRole, const FString& Portal, const FString& Options, const FUniqueNetIdRepl& UniqueId, FString& ErrorMessage)
{
    return This->Login(NewPlayer, InRemoteRole, Portal, Options, UniqueId, ErrorMessage);
}

APawn* AFortGameModeZone::SpawnDefaultPawnFor_ImplementationHK(AFortGameModeZone* This, AController* NewPlayer, AActor* StartSpot)
{
    return This->SpawnDefaultPawnFor_Implementation(NewPlayer, StartSpot);
}

void AFortGameModeZone::FinishWorldInitializationHK(AFortGameModeZone* This, AFortWorldManager* WorldManager)
{
    This->FinishWorldInitialization(WorldManager);
}

void AFortGameModeZone::SubclassFinishWorldInitializationHK(AFortGameModeZone* This, AFortWorldManager* WorldManager)
{
    const auto Original = SubclassFinishWorldInitialization.find(*reinterpret_cast<void***>(This));
    const bool bHasOriginal = Original != SubclassFinishWorldInitialization.end();
    // The outpost loads the core's placed buildings before the missions activate (Mission_Outpost.InitMission reads them back)
    const bool bOriginalFirst = Cast<AFortGameModeOutpost>(reinterpret_cast<UObject*>(This)) != nullptr;
    if (bHasOriginal && bOriginalFirst)
    {
        Original->second(This, WorldManager);
    }
    This->FinishWorldInitialization(WorldManager);
    // AFortGameModeDeployableBase's (5.0) hands over to the deployable base manager, whose plot spawn 5.0 strips: Core runs
    // the manager's part with the plots restored (CoreV2 #353).
    if (bHasOriginal && !bOriginalFirst && !AFortDeployableBaseManager::FinishWorldInitialization(This, reinterpret_cast<void*>(Original->second)))
    {
        Original->second(This, WorldManager);
    }
}
