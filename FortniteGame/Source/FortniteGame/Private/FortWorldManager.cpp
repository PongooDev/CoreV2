#include "pch.h"
#include "FortWorldManager.h"
#include "BlueprintGameplayTagLibrary.h"
#include "Building/BuildingActor.h"
#include "EnvironmentQuery/EnvQueryInstanceBlueprintWrapper.h"
#include "EnvironmentQuery/EnvQueryManager.h"
#include "GameThread.h"
#include "Math/Rotator.h"
#include "Missions/FortMissionManager.h"

#include <algorithm>
#include "Engine/World.h"
#include "FortGameData.h"
#include "FortGameMode.h"
#include "FortAssetManager.h"
#include "FortGameState.h"
#include "Finder.h"
#include "FortWorldRecord.h"
#include "Kismet/GameplayStatics.h"
#include "Logging/LogMacros.h"
#include "Hooks.h"
#include "Natives.h"
#include "Online/FortGameSession.h"
#include "Online/FortLobbyBeaconHost.h"
#include "Templates/Casts.h"
#include "UI/FrontEnd/FortMissionGenerator.h"

DEFINE_LOG_CATEGORY_STATIC(LogFortWorld, Log, All);

AActor* AFortWorldManager::SpawnPlayerSpawnPad(const FFortPlayerSpawnPadPlacementData& PlacementData, const FTransform& SpawnTransform)
{
#if WITH_SERVER_CODE
    UClass* ActorToPlace = PlacementData.ActorToPlace.LoadSynchronous();
    if (!ActorToPlace || !ActorToPlace->GetDefaultObject())
    {
        UE_LOG(LogFortWorld, Error, TEXT("SpawnPlayerSpawnPad: 'Actor To Place' has null CDO!  (I.E. Actor To Place is apparently unspecified.)  Cannot spawn player spawn pad."));
        return nullptr;
    }

    AActor* SpawnPad = UGameplayStatics::BeginDeferredActorSpawnFromClass(this, ActorToPlace, SpawnTransform, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
    if (SpawnPad)
    {
        SpawnPad = UGameplayStatics::FinishSpawningActor(SpawnPad, SpawnTransform);
    }

    if (!SpawnPad)
    {
        UE_LOG(LogFortWorld, Error, TEXT("SpawnPlayerSpawnPad: Failed to spawn actor %s."), *ActorToPlace->GetName());
    }
    return SpawnPad;
#else
    return nullptr;
#endif
}

namespace
{
    constexpr int32_t MissionPlacementQueryTicks = 600;

    void WaitForMissionPlacementQuery(FWeakObjectPtr WorldManager, FGuid MissionGuid, FWeakObjectPtr MissionInfo, int32_t ItemIndex, FWeakObjectPtr Query, int32_t TicksLeft)
    {
        GameThread::Enqueue([WorldManager, MissionGuid, MissionInfo, ItemIndex, Query, TicksLeft]()
        {
            AFortWorldManager* Manager = static_cast<AFortWorldManager*>(WorldManager.Get());
            UFortMissionInfo* Info = static_cast<UFortMissionInfo*>(MissionInfo.Get());
            UEnvQueryInstanceBlueprintWrapper* PlacementQuery = static_cast<UEnvQueryInstanceBlueprintWrapper*>(Query.Get());
            if (!Manager || !Info)
            {
                return;
            }

            const TArray<AActor*> Results = PlacementQuery ? PlacementQuery->GetResultsAsActors() : TArray<AActor*>();
            if (Results.Num() == 0 && TicksLeft > 0)
            {
                WaitForMissionPlacementQuery(WorldManager, MissionGuid, MissionInfo, ItemIndex, Query, TicksLeft - 1);
                return;
            }

            Manager->OnMissionPlacementQueryFinished(MissionGuid, Info, ItemIndex, Results);
        });
    }
}

void AFortWorldManager::OnWorldRecordQueryComplete()
{
    OnWorldRecordQueryCompleteOG(this);

#if WITH_SERVER_CODE
    if (WorldManagerState != EFortWorldManagerState::WMS_WorldQueryComplete)
    {
        return;
    }

    UWorld* World = GetWorld();
    AFortGameMode* GameMode = World ? Cast<AFortGameMode>(reinterpret_cast<UObject*>(World->GetAuthGameMode())) : nullptr;
    AFortGameSession* GameSession = GameMode ? GameMode->FortGameSession : nullptr;
    if (AFortLobbyBeaconHost* LobbyBeaconHost = GameSession ? GameSession->LobbyBeaconHost : nullptr)
    {
        LobbyBeaconHost->SetWorldRecordLoaded(true);
    }
#endif
}

void AFortWorldManager::ServerTickLoadingOrCreatingWorld()
{
#if WITH_SERVER_CODE
    if (!bHasCreatedMissions)
    {
        if (!bCreatedMissionRecord)
        {
            UWorld* World = GetWorld();
            AFortGameMode* GameMode = World ? Cast<AFortGameMode>(reinterpret_cast<UObject*>(World->GetAuthGameMode())) : nullptr;

            UScriptStruct* RecordStruct = FFortMissionManagerRecord::StaticStruct();
            std::vector<uint8_t> RecordStorage(RecordStruct->GetStructureSize(), 0);
            FFortMissionManagerRecord* Record = reinterpret_cast<FFortMissionManagerRecord*>(RecordStorage.data());
            FFortMissionManagerRecord::Construct(Record, GameMode ? static_cast<UClass*>(GameMode->MissionManagerClass) : nullptr);
            RecordStruct->CopyScriptStruct(&static_cast<FFortMissionManagerRecord&>(MissionManagerRecord), Record);
            RecordStruct->DestroyStruct(Record);
            bCreatedMissionRecord = true;

            FFortStartingMissionInfo& StartingMissions = StartingMissionInfo;
            TArray<UFortMissionInfo*>& StartingMissionList = StartingMissions.StartingMissions;
            AFortGameStateZone* GameState = World ? Cast<AFortGameStateZone>(reinterpret_cast<UObject*>(World->GetGameState())) : nullptr;
            UFortMissionGenerator* MissionGenerator = GameState ? GameState->GetMissionGenerator() : nullptr;
            if (StartingMissionList.Num() > 0 || !MissionGenerator)
            {
                AddMissionsToMissionManagerRecord(StartingMissionList);
            }
            else
            {
                const TAssetPtr<UFortMissionInfo>& PrimaryMissionInfo = MissionGenerator->PrimaryMissionInfo;
                if (UFortMissionInfo* PrimaryMission = PrimaryMissionInfo.LoadSynchronous())
                {
                    AddMissionsToMissionManagerRecord({ PrimaryMission }, EMissionGenerationCategory::Primary, MissionGenerator);
                }
            }
        }
        bHasCreatedMissions = true;
    }
#endif

    ServerTickLoadingOrCreatingWorldOG(this);
}

void AFortWorldManager::AddMissionsToMissionManagerRecord(const TArray<UFortMissionInfo*>& Missions, EMissionGenerationCategory GenerationCategory, UFortMissionGenerator* MissionGenerator)
{
#if WITH_SERVER_CODE
    if (!bCreatedMissionRecord)
    {
        UE_LOG(LogFortWorld, Error, TEXT("Trying to add mission manager records before the mission manager record has been created."));
        return;
    }

    UWorld* World = GetWorld();
    AFortGameStateZone* GameState = World ? Cast<AFortGameStateZone>(reinterpret_cast<UObject*>(World->GetGameState())) : nullptr;
    const float GameDifficulty = GameState ? static_cast<float>(GameState->GameDifficulty) : 0.0f;
    AGameModeBase* GameMode = World ? World->GetAuthGameMode() : nullptr;
    UFortGameData* GameData = UFortAssetManager::Get().GetGameData();
    if (Natives::UFortGameData_MissionGenerationData < 0)
    {
        Finder::ResolveMissionGenerationLive();
    }
    const TAssetPtr<UObject>* GenerationDataPtr = GameData && Natives::UFortGameData_MissionGenerationData > 0 ? reinterpret_cast<const TAssetPtr<UObject>*>(reinterpret_cast<uint8_t*>(GameData) + Natives::UFortGameData_MissionGenerationData) : nullptr;
    UObject* GenerationData = GenerationDataPtr ? GenerationDataPtr->LoadSynchronous() : nullptr;
    void* RandomStream = Natives::AFortWorldManager_MissionGenerationRandomStream > 0 ? reinterpret_cast<uint8_t*>(this) + Natives::AFortWorldManager_MissionGenerationRandomStream : nullptr;

    FFortMissionManagerRecord& Record = MissionManagerRecord;
    TStructArray<FFortMissionRecord>& MissionRecords = Record.MissionRecords;
    for (int32_t Index = 0; Index < Missions.Num(); ++Index)
    {
        UFortMissionInfo* MissionInfo = Missions[Index];
        if (!MissionInfo)
        {
            continue;
        }

        FFortMissionRecord& MissionRecord = MissionRecords.AddDefaulted();
        FFortMissionEntry& MissionEntry = MissionRecord.MissionEntry;
        MissionEntry.Weight = 1.0f;
        MissionEntry.WorldMinLevel = -1;
        MissionEntry.WorldMaxLevel = -1;
        MissionEntry.MissionGenerator = MissionGenerator;
        MissionEntry.MissionInfo = MissionInfo;
        MissionEntry.GenerationCategory = GenerationCategory;
        if (GenerationData && RandomStream)
        {
            FFortGeneratedDifficultyOptions& GeneratedDifficultyOptions = MissionEntry.GeneratedDifficultyOptions;
            MissionInfo->GenerateMissionDifficultyOptions(GeneratedDifficultyOptions, RandomStream, GameDifficulty, GameMode, GenerationData);
        }
        MissionRecord.MissionGenerator = MissionGenerator;
        MissionRecord.MissionGuid = FGuid::NewGuid();
        PlaceMissionItems(MissionRecord.MissionGuid, MissionInfo);
    }
#endif
}

void AFortWorldManager::PlaceMissionItems(const FGuid& MissionGuid, UFortMissionInfo* MissionInfo)
{
#if WITH_SERVER_CODE
    FFortMissionPlacementItems& PlacementInfo = MissionInfo->MissionPlacementInfo;
    TStructArray<FFortMissionPlacementActorItem>& ActorItems = PlacementInfo.ActorsAndLocations;
    for (int32_t ItemIndex = 0; ItemIndex < ActorItems.Num(); ++ItemIndex)
    {
        UEnvQuery* PlacementQuery = ActorItems[ItemIndex].PlacementQuery;
        UEnvQueryInstanceBlueprintWrapper* Query = PlacementQuery ? UEnvQueryManager::RunEQSQuery(this, PlacementQuery, this, EEnvQueryRunMode::AllMatching, nullptr) : nullptr;
        if (!Query)
        {
            UE_LOG(LogFortWorld, Warning, TEXT("PlaceMissionItems: no placement query for item %d of mission %s"), ItemIndex, *MissionInfo->GetName());
            continue;
        }
        WaitForMissionPlacementQuery(FWeakObjectPtr(this), MissionGuid, FWeakObjectPtr(MissionInfo), ItemIndex, FWeakObjectPtr(Query), MissionPlacementQueryTicks);
    }
#endif
}

void AFortWorldManager::OnMissionPlacementQueryFinished(const FGuid& MissionGuid, UFortMissionInfo* MissionInfo, int32_t ItemIndex, const TArray<AActor*>& PlacementActors)
{
#if WITH_SERVER_CODE
    FFortMissionRecord* MissionRecord = nullptr;
    TStructArray<FFortMissionRecord>& MissionRecords = static_cast<FFortMissionManagerRecord&>(MissionManagerRecord).MissionRecords;
    for (int32_t Index = 0; Index < MissionRecords.Num() && !MissionRecord; ++Index)
    {
        MissionRecord = static_cast<const FGuid&>(MissionRecords[Index].MissionGuid) == MissionGuid ? &MissionRecords[Index] : nullptr;
    }
    FFortMissionPlacementItems& PlacementInfo = MissionInfo->MissionPlacementInfo;
    TStructArray<FFortMissionPlacementActorItem>& ActorItems = PlacementInfo.ActorsAndLocations;
    if (!MissionRecord || ItemIndex >= ActorItems.Num())
    {
        return;
    }

    FFortMissionPlacementActorItem& Item = ActorItems[ItemIndex];
    const int32_t NumLocations = std::max(static_cast<int32_t>(Item.NumLocationsToFind), 1);
    UClass* ActorClass = static_cast<TAssetSubclassOf<AActor>&>(Item.ActorToPlace).LoadSynchronous();
    if (PlacementActors.Num() < NumLocations)
    {
        UE_LOG(LogFortWorld, Warning, TEXT("PlaceMissionItems: mission %s item %d found %d of %d locations"), *MissionInfo->GetName(), ItemIndex, PlacementActors.Num(), NumLocations);
    }

    FFortMissionEntry& MissionEntry = MissionRecord->MissionEntry;
    TStructArray<FFortMissionPlacementItemLookupData>& LookupData = MissionEntry.BlueprintLookupData;
    UWorld* World = GetWorld();
    for (int32_t Index = 0; Index < PlacementActors.Num() && Index < NumLocations; ++Index)
    {
        AActor* PlacementActor = PlacementActors[Index];
        if (!PlacementActor)
        {
            continue;
        }
        if (ABuildingActor* PlacementBuilding = Cast<ABuildingActor>(reinterpret_cast<UObject*>(PlacementActor)))
        {
            FGameplayTagContainer& PlacementTags = PlacementBuilding->StaticGameplayTags;
            UBlueprintGameplayTagLibrary::AppendGameplayTagContainers(PlacementTags, PlacementInfo.TagsToAddToChosenPlacementActors);
            UBlueprintGameplayTagLibrary::AppendGameplayTagContainers(PlacementTags, Item.TagsToAddToChosenPlacementActor);
        }

        FTransform SpawnTransform = PlacementActor->GetTransform();
        FRotator SpawnRotation = SpawnTransform.Rotation.Rotator();
        if (Item.bSnapToGrid)
        {
            SpawnRotation = FRotator(0.0f, std::round(SpawnRotation.Yaw / 90.0f) * 90.0f, 0.0f);
            SpawnTransform.Rotation = SpawnRotation.Quaternion();
        }
        FFortMissionPlacementItemLookupData& Data = LookupData.AddDefaulted();
        UBlueprintGameplayTagLibrary::AppendGameplayTagContainers(Data.ItemIdentifyingTags, Item.ItemIdentifyingTags);
        UBlueprintGameplayTagLibrary::AppendGameplayTagContainers(Data.TagsAddedToPlacementActors, Item.TagsToAddToChosenPlacementActor);
        Data.ActorToPlace = ActorClass;
        Data.ActorToUseForSpawnLocation = PlacementActor;
        Data.SpawnLocation = SpawnTransform.GetLocation();
        Data.SpawnRotation = SpawnRotation;
        if (Item.bSpawnActorAutomatically && ActorClass && World)
        {
            Data.SpawnedActor = World->SpawnActor(ActorClass, &SpawnTransform, FActorSpawnParameters());
        }
    }
    UE_LOG(LogFortWorld, Log, TEXT("PlaceMissionItems: mission %s item %d placed %d"), *MissionInfo->GetName(), ItemIndex, std::min(PlacementActors.Num(), NumLocations));
#endif
}

void AFortWorldManager::CreateMissionManager()
{
#if WITH_SERVER_CODE
    UWorld* World = GetWorld();
    AFortGameState* GameState = World ? Cast<AFortGameState>(reinterpret_cast<UObject*>(World->GetGameState())) : nullptr;
    if (!GameState || !bCreatedMissionRecord || GameState->MissionManager)
    {
        return;
    }

    GameState->SpawnMissionManager(MissionManagerRecord);
#endif
}

ptrdiff_t AFortWorldManager::ZoneFinishedOffset()
{
    return Member_bZoneFinished.GetOffsetChecked();
}

ptrdiff_t AFortWorldManager::ZoneCompletedOffset()
{
    return Member_bZoneCompleted.GetOffsetChecked();
}

int32_t AFortWorldManager::GetOrAllocatePlayerID(const FString& UniqueId, const FString& DisplayName)
{
#if WITH_SERVER_CODE
    UFortWorldRecord* WorldRecord = CurrentWorldRecord;
    if (!WorldRecord || UniqueId.IsEmpty())
    {
        return -1;
    }

    TStructArray<FFortPlayerRecord>& SavedPlayers = WorldRecord->SavedPlayers;
    for (int32_t Index = 0; Index < SavedPlayers.Num(); ++Index)
    {
        FString& SavedUniqueId = SavedPlayers[Index].UniqueId;
        if (_wcsicmp(*SavedUniqueId, *UniqueId) == 0)
        {
            return Index;
        }
    }

    FFortPlayerRecord& PlayerRecord = SavedPlayers.AddDefaulted();
    PlayerRecord.UniqueId = UniqueId;
    PlayerRecord.DisplayName = DisplayName;
    PlayerRecord.bPlayerIsNew = true;
    return SavedPlayers.Num() - 1;
#else
    return -1;
#endif
}

void AFortWorldManager::Hook()
{
    Hooks::Detour(L"AFortWorldManager::OnWorldRecordQueryComplete", reinterpret_cast<void*>(Natives::AFortWorldManager_OnWorldRecordQueryComplete), &OnWorldRecordQueryCompleteHK, &OnWorldRecordQueryCompleteOG);
    Hooks::Detour(L"AFortWorldManager::ServerTickLoadingOrCreatingWorld", reinterpret_cast<void*>(Natives::AFortWorldManager_ServerTickLoadingOrCreatingWorld), &ServerTickLoadingOrCreatingWorldHK, &ServerTickLoadingOrCreatingWorldOG);
}

void AFortWorldManager::OnWorldRecordQueryCompleteHK(AFortWorldManager* This)
{
    This->OnWorldRecordQueryComplete();
}

void AFortWorldManager::ServerTickLoadingOrCreatingWorldHK(AFortWorldManager* This)
{
    This->ServerTickLoadingOrCreatingWorld();
}
