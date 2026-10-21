#include "pch.h"
#include "FortWorldManager.h"
#include "Missions/FortMissionManager.h"
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
                AddMissionsToMissionManagerRecord(StartingMissionList, EMissionGenerationCategory::Primary);
            }
            else
            {
                const TAssetPtr<UFortMissionInfo>& PrimaryMissionInfo = MissionGenerator->PrimaryMissionInfo;
                if (UFortMissionInfo* PrimaryMission = PrimaryMissionInfo.LoadSynchronous())
                {
                    AddMissionsToMissionManagerRecord({ PrimaryMission }, EMissionGenerationCategory::Primary, MissionGenerator);
                }

                const std::pair<TStructArray<FFortPossibleMission>*, EMissionGenerationCategory> Lists[] = {
                    { &static_cast<TStructArray<FFortPossibleMission>&>(MissionGenerator->SecondaryMissionList), EMissionGenerationCategory::Secondary },
                    { &static_cast<TStructArray<FFortPossibleMission>&>(MissionGenerator->TertiaryMissionList), EMissionGenerationCategory::Tertiary },
                    { &static_cast<TStructArray<FFortPossibleMission>&>(MissionGenerator->SurvivorMissionList), EMissionGenerationCategory::Survivor },
                };
                for (const auto& [PossibleMissions, Category] : Lists)
                {
                    TArray<UFortMissionInfo*> Generated;
                    for (int32_t Index = 0; Index < PossibleMissions->Num(); ++Index)
                    {
                        FFortPossibleMission& Possible = (*PossibleMissions)[Index];
                        const TAssetPtr<UFortMissionInfo>& PossibleInfo = Possible.MissionInfo;
                        UFortMissionInfo* Info = Possible.bIsPrototype ? nullptr : PossibleInfo.LoadSynchronous();
                        for (int32_t Count = 0; Info && Count < Possible.MinAlwaysGenerated; ++Count)
                        {
                            Generated.Add(Info);
                        }
                    }
                    if (Generated.Num() > 0)
                    {
                        AddMissionsToMissionManagerRecord(Generated, Category, MissionGenerator);
                    }
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
