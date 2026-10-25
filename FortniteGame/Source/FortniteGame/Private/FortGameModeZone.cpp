#include "pch.h"
#include "FortGameModeZone.h"
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
#include "UI/FrontEnd/FortMissionGenerator.h"
#include "GameFramework/OnlineReplStructs.h"
#include "Player/FortPlayerController.h"
#include "Player/FortPlayerState.h"
#include "FortWorldManager.h"
#include "GameThread.h"
#include "Logging/LogMacros.h"
#include "Building/BuildingActor.h"
#include "Building/BuildingStructuralSupportSystem.h"
#include "Templates/Casts.h"
#include "UObject/WeakObjectPtr.h"
#include "Finder.h"
#include "Hooks.h"
#include "Log.h"
#include "Natives.h"
#include "ObjectLayout.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <unordered_map>
#include <vector>

APawn* AFortGameModeZone::SpawnDefaultPawnFor_Implementation(AController* NewPlayer, AActor* StartSpot)
{
#if WITH_SERVER_CODE
    if (!StartSpot || !NewPlayer)
    {
        return nullptr;
    }

    return AFortGameMode::SpawnDefaultPawnFor_Implementation(NewPlayer, StartSpot);
#else
    return nullptr;
#endif
}

DEFINE_LOG_CATEGORY_STATIC(LogFortPlacement, Log, All);
DEFINE_LOG_CATEGORY_STATIC(LogNavigation, Log, All);

namespace
{
    constexpr int32_t SpawnPadQueryTicks = 600;
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

    void WaitForQuery(FWeakObjectPtr GameMode, FWeakObjectPtr WorldManager, FWeakObjectPtr Query, int32_t TicksLeft)
    {
        GameThread::Enqueue([GameMode, WorldManager, Query, TicksLeft]()
        {
            AFortGameModeZone* Zone = static_cast<AFortGameModeZone*>(GameMode.Get());
            UEnvQueryInstanceBlueprintWrapper* PlacementQuery = static_cast<UEnvQueryInstanceBlueprintWrapper*>(Query.Get());
            if (!Zone)
            {
                return;
            }

            if (PlacementQuery && PlacementQuery->GetResultsAsActors().Num() == 0 && TicksLeft > 0)
            {
                WaitForQuery(GameMode, WorldManager, Query, TicksLeft - 1);
                return;
            }

            Zone->OnPlayerSpawnPadQueryFinished(static_cast<AFortWorldManager*>(WorldManager.Get()), PlacementQuery);
        });
    }
}

void AFortGameModeZone::FinishWorldInitialization(AFortWorldManager* WorldManager)
{
#if WITH_SERVER_CODE
    PlacePlayerSpawnPad(WorldManager);
    if (WorldManager)
    {
        WorldManager->CreateMissionManager();
    }
    AFortGameMode::FinishWorldInitialization(WorldManager);

    UWorld* World = GetWorld();
    UNavigationSystem* NavSys = World ? World->GetNavigationSystem() : nullptr;
    if (NavSys)
    {
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
                continue;
            }
            if (USceneComponent* Root = NavBounds->GetRootComponent())
            {
                Root->Mobility = EComponentMobility::Movable;
            }
            NavBounds->SetActorScale3D(WorldMax - WorldMin);
            NavBounds->SetActorLocation((WorldMin + WorldMax) * 0.5f);
            NavSys->OnNavigationBoundsUpdated(NavBounds);
        }

        const double Started = NowMs();
        NavSys->ReleaseInitialBuildingLock();
        UE_LOG(LogNavigation, Log, TEXT("Navmesh RebuildAll time:%.3fms"), NowMs() - Started);
    }

    if (AFortNavigationGraph* NavGraph = AFortNavigationGraph::GetCurrent(World))
    {
        NavGraph->OnWorldInitialized();
    }
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
        AIDirectorClass = Index >= 0 && Index < AIDirectors.Num() ? AIDirectors[Index].LoadSynchronous() : nullptr;
    }
    return AIDirectorClass && AIDirectorClass->IsChildOf(AFortAIDirector::StaticClass()) ? AIDirectorClass : AFortAIDirector::StaticClass();
}

void AFortGameModeZone::CreateAIDirector()
{
#if WITH_SERVER_CODE
    UWorld* World = GetWorld();
    if (World)
    {
        AIDirector = World->SpawnActor<AFortAIDirector>(DetermineAIDirectorClass(), FActorSpawnParameters());
    }
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
    if (PlayerState && WorldManager)
    {
        FString& PlayerName = PlayerState->PlayerName;
        PlayerState->WorldPlayerId = WorldManager->GetOrAllocatePlayerID(UniqueId.ToString(), PlayerName);
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

    WaitForQuery(FWeakObjectPtr(this), FWeakObjectPtr(WorldManager), FWeakObjectPtr(Query), SpawnPadQueryTicks);
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
        const FGameplayTagContainer& DefaultTags = PlacementActor->DefaultTags;
        if (!std::all_of(RequiredTags.begin(), RequiredTags.end(), [&](FName Tag) { return HasPlacementTag(StaticTags, Tag) || HasPlacementTag(DefaultTags, Tag); }))
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
    if (This && Candidates)
    {
        This->GatherPlayerSpawnActors(Player, false, false, *Candidates);
    }
    return Natives::AFortGameMode_PickPlayerSpawnActor(This, Player, PawnClass, Candidates, DangerExclusionRadius, ObjectiveExclusionRadius);
}

namespace
{
    using FFinishWorldInitialization = void (*)(AFortGameModeZone* This, AFortWorldManager* WorldManager);
    std::unordered_map<void**, FFinishWorldInitialization> SubclassFinishWorldInitialization;
}

void AFortGameModeZone::HookVTables()
{
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
            }
        }
    }

    if (Finder::ResolveLoginLive())
    {
        const int32_t LoginIndex = Natives::AGameModeBase_LoginIndex;
        UObject* ZoneDefault = StaticClass()->GetDefaultObject(false);
        void* const ZoneLogin = ZoneDefault ? (*reinterpret_cast<void***>(ZoneDefault))[LoginIndex] : nullptr;
        LoginOG = reinterpret_cast<decltype(LoginOG)>(ZoneLogin);
        for (TObjectIterator<UClass> It; ZoneLogin && It; ++It)
        {
            UClass* Class = *It;
            UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
            void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
            if (VTable && VTable[LoginIndex] == ZoneLogin)
            {
                Hooks::SwapVTable(L"AFortGameModeZone::Login", VTable, LoginIndex, reinterpret_cast<void*>(&LoginHK));
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
    if (bHasOriginal && !bOriginalFirst)
    {
        Original->second(This, WorldManager);
    }
}
