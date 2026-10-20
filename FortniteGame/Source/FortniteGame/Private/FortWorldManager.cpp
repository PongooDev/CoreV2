#include "pch.h"
#include "FortWorldManager.h"
#include "Engine/World.h"
#include "FortFastLoadConfig.h"
#include "FortGameData.h"
#include "FortGameMode.h"
#include "FortGameState.h"
#include "FortWorldRecord.h"
#include "Kismet/GameplayStatics.h"
#include "Logging/LogMacros.h"
#include "Hooks.h"
#include "Natives.h"
#include "Online/FortGameSession.h"
#include "Online/FortLobbyBeaconHost.h"
#include "Templates/Casts.h"

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

            UFortFastLoadConfig* FastLoadConfig = static_cast<UFortFastLoadConfig*>(UFortFastLoadConfig::StaticClass()->GetDefaultObject());
            if (!FastLoadConfig || !FastLoadConfig->bDisableStartingMissions)
            {
                FFortStartingMissionInfo& StartingMissions = StartingMissionInfo;
                AddMissionsToMissionManagerRecord(StartingMissions.StartingMissions);
            }
        }
        bHasCreatedMissions = true;
    }
#endif

    ServerTickLoadingOrCreatingWorldOG(this);
}

void AFortWorldManager::AddMissionsToMissionManagerRecord(const TArray<UFortMissionInfo*>& Missions)
{
#if WITH_SERVER_CODE
    if (!bCreatedMissionRecord)
    {
        UE_LOG(LogFortWorld, Error, TEXT("Trying to add mission manager records before the mission manager record has been created."));
        return;
    }

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
        MissionEntry.MissionInfo = MissionInfo;
        MissionEntry.GenerationCategory = EMissionGenerationCategory::Max_None;
        MissionRecord.MissionGuid = FGuid::NewGuid();
    }
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
