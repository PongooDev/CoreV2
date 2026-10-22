#include "pch.h"
#include "FortWorldManager.h"
#include "Missions/FortMissionManager.h"
#include "BlueprintGameplayTagLibrary.h"
#include "Engine/World.h"
#include "FortGameData.h"
#include "FortGameInstance.h"
#include "FortGameMode.h"
#include "FortAssetManager.h"
#include "FortGameState.h"
#include "FortTheaterInfo.h"
#include "FortZoneTheme.h"
#include "Finder.h"
#include "FortWorldRecord.h"
#include "Kismet/GameplayStatics.h"
#include "Logging/LogMacros.h"
#include "Hooks.h"
#include "Items/FortInventory.h"
#include "Items/FortItemDefinition.h"
#include "Items/FortQuickBars.h"
#include "Missions/FortSharedMissionLists.h"
#include "Natives.h"
#include "Online/FortGameSession.h"
#include "Online/FortLobbyBeaconHost.h"
#include "Online/FortMcpProfileWorld.h"
#include "Player/FortPlayerController.h"
#include "Player/FortPlayerState.h"
#include "Quests/FortQuestManager.h"
#include "Templates/Casts.h"
#include "UI/FrontEnd/FortMissionGenerator.h"

#include <vector>

DEFINE_LOG_CATEGORY_STATIC(LogFortWorld, Log, All);

namespace
{
    float RandomStreamFraction(void* RandomStream)
    {
        if (!RandomStream)
        {
            return Natives::FMath_SRand();
        }
        uint8_t* Stream = static_cast<uint8_t*>(RandomStream);
        const float* Values = *reinterpret_cast<float**>(Stream);
        int32_t& Index = *reinterpret_cast<int32_t*>(Stream + 0x10);
        const int32_t Num = *reinterpret_cast<int32_t*>(Stream + 0x14);
        if (Num <= 0 || !Values)
        {
            return 0.0f;
        }
        if (Index >= Num)
        {
            Index = 0;
        }
        return Values[Index++];
    }

    bool MatchesQuery(const FGameplayTagContainer& Tags, const FGameplayTagQuery& Query)
    {
        return Query.IsEmpty() || UBlueprintGameplayTagLibrary::DoesContainerMatchTagQuery(Tags, Query);
    }
}

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
    if (WorldManagerState != EFortWorldManagerState::GetWMS_WorldQueryComplete())
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
            if (!MissionGenerator)
            {
                AddMissionsToMissionManagerRecord(StartingMissionList, EMissionGenerationCategory::GetPrimary());
            }
            else
            {
                const TAssetPtr<UFortMissionInfo>& PrimaryMissionInfo = MissionGenerator->PrimaryMissionInfo;
                if (UFortMissionInfo* PrimaryMission = PrimaryMissionInfo.LoadSynchronous())
                {
                    UE_LOG(LogFortWorld, Log, TEXT("Mission Generator %s adds Primary mission %s"), *MissionGenerator->GetName(), *PrimaryMission->GetName());
                    AddMissionsToMissionManagerRecord({ PrimaryMission }, EMissionGenerationCategory::GetPrimary(), MissionGenerator);
                }

                if (!Natives::AFortWorldManager_GetTheaterMapData)
                {
                    Finder::ResolveTheaterMapDataLive();
                }
                FFortTheaterMapData* TheaterMapData = Natives::AFortWorldManager_GetTheaterMapData ? static_cast<FFortTheaterMapData*>(Natives::AFortWorldManager_GetTheaterMapData(this)) : nullptr;
                const bool bAllowPrototypes = TheaterMapData && TheaterMapData->RuntimeInfo.TheaterType == EFortTheaterType::GetTest();

                const std::pair<TStructArray<FFortPossibleMission>*, EMissionGenerationCategory> Lists[] = {
                    { &static_cast<TStructArray<FFortPossibleMission>&>(MissionGenerator->SecondaryMissionList), EMissionGenerationCategory::GetSecondary() },
                    { &static_cast<TStructArray<FFortPossibleMission>&>(MissionGenerator->TertiaryMissionList), EMissionGenerationCategory::GetTertiary() },
                    { &static_cast<TStructArray<FFortPossibleMission>&>(MissionGenerator->SurvivorMissionList), EMissionGenerationCategory::GetSurvivor() },
                };
                void* RandomStream = Natives::AFortWorldManager_MissionGenerationRandomStream > 0 ? reinterpret_cast<uint8_t*>(this) + Natives::AFortWorldManager_MissionGenerationRandomStream : nullptr;
                FFortMissionManagerRecord& ManagerRecord = MissionManagerRecord;
                for (const auto& [PossibleMissions, Category] : Lists)
                {
                    TArray<UFortMissionInfo*> Generated;
                    float TotalWeight = 0.0f;
                    for (int32_t Index = 0; Index < PossibleMissions->Num(); ++Index)
                    {
                        FFortPossibleMission& Possible = (*PossibleMissions)[Index];
                        const TAssetPtr<UFortMissionInfo>& PossibleInfo = Possible.MissionInfo;
                        const bool bExcluded = Possible.bIsPrototype && !bAllowPrototypes;
                        UFortMissionInfo* Info = bExcluded ? nullptr : PossibleInfo.LoadSynchronous();
                        for (int32_t Count = 0; Info && Count < Possible.MinAlwaysGenerated; ++Count)
                        {
                            Generated.Add(Info);
                        }
                        TotalWeight += !bExcluded && Possible.Weight > 0.0f ? static_cast<float>(Possible.Weight) : 0.0f;
                    }
                    (&static_cast<int32_t&>(ManagerRecord.NumRequiredMissionsOfType))[static_cast<int32_t>(Category)] = Generated.Num();

                    const float Chance = (&static_cast<float&>(MissionGenerator->MissionGenerationChance))[static_cast<int32_t>(Category)];
                    const int32_t WholePicks = static_cast<int32_t>(Chance);
                    const int32_t Picks = Chance > 0.0f ? WholePicks + (RandomStreamFraction(RandomStream) < Chance - static_cast<float>(WholePicks) ? 1 : 0) : 0;
                    if (Picks > 0 && TotalWeight <= 0.0f)
                    {
                        UE_LOG(LogFortWorld, Warning, TEXT("Mission Generator %s group %d has no valid weights "), *MissionGenerator->GetName(), static_cast<int32_t>(Category));
                    }
                    for (int32_t Pick = 0; Pick < Picks && TotalWeight > 0.0f; ++Pick)
                    {
                        float Roll = RandomStreamFraction(RandomStream) * TotalWeight;
                        int32_t Selected = -1;
                        for (int32_t Index = 0; Index < PossibleMissions->Num() && Selected < 0; ++Index)
                        {
                            FFortPossibleMission& Possible = (*PossibleMissions)[Index];
                            const float Weight = !(Possible.bIsPrototype && !bAllowPrototypes) && Possible.Weight > 0.0f ? static_cast<float>(Possible.Weight) : 0.0f;
                            Selected = Weight > 0.0f && Roll < Weight ? Index : -1;
                            Roll -= Weight;
                        }
                        const TAssetPtr<UFortMissionInfo>& SelectedInfo = Selected >= 0 ? static_cast<const TAssetPtr<UFortMissionInfo>&>((*PossibleMissions)[Selected].MissionInfo) : TAssetPtr<UFortMissionInfo>();
                        if (UFortMissionInfo* Info = Selected >= 0 ? SelectedInfo.LoadSynchronous() : nullptr)
                        {
                            Generated.Add(Info);
                        }
                        else
                        {
                            UE_LOG(LogFortWorld, Warning, TEXT("Mission Generator %s group %d selected invalid mission index %d"), *MissionGenerator->GetName(), static_cast<int32_t>(Category), Selected);
                        }
                    }
                    for (UFortMissionInfo* Info : Generated)
                    {
                        UE_LOG(LogFortWorld, Log, TEXT("Mission Generator %s adds %s mission %s"), *MissionGenerator->GetName(), Category.ToString().c_str(), *Info->GetName());
                    }
                    if (Generated.Num() > 0)
                    {
                        AddMissionsToMissionManagerRecord(Generated, Category, MissionGenerator);
                    }
                }
            }

            if (!StartingMissions.bDisableSharedMissionLoading)
            {
                AddSharedMissionsToMissionManagerRecord();
            }
        }
        bHasCreatedMissions = true;
    }
#endif

    ServerTickLoadingOrCreatingWorldOG(this);
}

void AFortWorldManager::AddSharedMissionsToMissionManagerRecord()
{
#if WITH_SERVER_CODE
    UWorld* World = GetWorld();
    AFortGameMode* GameMode = World ? Cast<AFortGameMode>(reinterpret_cast<UObject*>(World->GetAuthGameMode())) : nullptr;
    UFortSharedMissionLists* SharedMissionLists = GameMode ? GameMode->GetSharedMissionLists() : nullptr;
    if (!SharedMissionLists)
    {
        return;
    }

    AFortGameStateZone* GameState = Cast<AFortGameStateZone>(reinterpret_cast<UObject*>(World->GetGameState()));
    const float GameDifficulty = GameState ? static_cast<float>(GameState->GameDifficulty) : 0.0f;
    UFortZoneTheme* ZoneTheme = GameState ? static_cast<UFortZoneTheme*>(GameState->ZoneTheme) : nullptr;
    const FGameplayTagContainer& GameContextTags = GameMode->GameContextTags;

    if (!Natives::AFortWorldManager_GetTheaterMapData)
    {
        Finder::ResolveTheaterMapDataLive();
    }
    FFortTheaterMapData* TheaterMapData = Natives::AFortWorldManager_GetTheaterMapData ? static_cast<FFortTheaterMapData*>(Natives::AFortWorldManager_GetTheaterMapData(this)) : nullptr;
    const EFortTheaterType TheaterType = TheaterMapData ? TheaterMapData->RuntimeInfo.TheaterType : EFortTheaterType::GetMax_None();
    const bool bIsTestMap = TheaterType == EFortTheaterType::GetTest() || TheaterType == EFortTheaterType::GetMax_None();

    FFortMissionManagerRecord& Record = MissionManagerRecord;
    TStructArray<FFortMissionRecord>& MissionRecords = Record.MissionRecords;
    const int32_t NumGeneratedRecords = MissionRecords.Num();
    FGameplayTagContainer PrimaryMissionTags;
    for (int32_t Index = 0; Index < NumGeneratedRecords; ++Index)
    {
        FFortMissionEntry& MissionEntry = MissionRecords[Index].MissionEntry;
        UFortMissionInfo* MissionInfo = MissionEntry.MissionInfo;
        if (MissionInfo && MissionInfo->MissionType == EFortMissionType::GetPrimary())
        {
            UBlueprintGameplayTagLibrary::AppendGameplayTagContainers(PrimaryMissionTags, MissionInfo->MissionTags);
        }
    }

    auto AllowedGlobalMission = [&](FFortGlobalMission& SharedMission) -> UFortMissionInfo*
    {
        if (!MatchesQuery(PrimaryMissionTags, SharedMission.PrimaryMissionTagQuery))
        {
            return nullptr;
        }

        const float MinDifficultyLevel = SharedMission.MinDifficultyLevel;
        const float MaxDifficultyLevel = SharedMission.MaxDifficultyLevel;
        if ((MinDifficultyLevel >= 0.0f && GameDifficulty < MinDifficultyLevel) || (MaxDifficultyLevel >= 0.0f && GameDifficulty > MaxDifficultyLevel))
        {
            return nullptr;
        }

        if ((SharedMission.bIsPrototype && TheaterType != EFortTheaterType::GetTest()) || (bIsTestMap && !SharedMission.bAllowInTestMaps))
        {
            return nullptr;
        }

        if (TheaterMapData)
        {
            const TArray<EFortTheaterType>& AllowedTheaterTypes = SharedMission.AllowedTheaterTypes;
            if ((AllowedTheaterTypes.Num() > 0 && !AllowedTheaterTypes.Contains(TheaterType)) || !MatchesQuery(TheaterMapData->RuntimeInfo.TheaterTags, SharedMission.TheaterTagQuery))
            {
                return nullptr;
            }
        }

        if (ZoneTheme)
        {
            const TArray<EFortZoneType>& AllowedZoneTypes = SharedMission.AllowedZoneTypes;
            if ((AllowedZoneTypes.Num() > 0 && !AllowedZoneTypes.Contains(ZoneTheme->ZoneType)) || !MatchesQuery(ZoneTheme->ZoneTags, SharedMission.ZoneTagQuery))
            {
                return nullptr;
            }
        }

        if (!MatchesQuery(GameContextTags, SharedMission.RegionTagQuery))
        {
            return nullptr;
        }

        const TAssetPtr<UFortMissionInfo>& SharedMissionInfo = SharedMission.MissionInfo;
        UFortMissionInfo* MissionInfo = SharedMissionInfo.LoadSynchronous();
        bool bAlreadyGenerated = false;
        for (int32_t RecordIndex = 0; MissionInfo && RecordIndex < NumGeneratedRecords && !bAlreadyGenerated; ++RecordIndex)
        {
            FFortMissionEntry& MissionEntry = MissionRecords[RecordIndex].MissionEntry;
            bAlreadyGenerated = static_cast<UFortMissionInfo*>(MissionEntry.MissionInfo) == MissionInfo;
        }
        return MissionInfo && !bAlreadyGenerated ? MissionInfo : nullptr;
    };

    TArray<UFortMissionInfo*> SharedMissions;
    TStructArray<FFortGlobalMission>& SharedMissionList = SharedMissionLists->SharedMissionList;
    for (int32_t Index = 0; Index < SharedMissionList.Num(); ++Index)
    {
        if (UFortMissionInfo* MissionInfo = AllowedGlobalMission(SharedMissionList[Index]))
        {
            SharedMissions.Add(MissionInfo);
        }
    }

    UFortGameInstance* GameInstance = Cast<UFortGameInstance>(reinterpret_cast<UObject*>(World->OwningGameInstance));
    TArray<UFortRegisteredPlayerInfo*> RegisteredPlayers;
    if (GameInstance)
    {
        RegisteredPlayers = GameInstance->RegisteredPlayers;
    }
    TStructArray<FFortQuestDrivenMission>& QuestDrivenMissionList = SharedMissionLists->QuestDrivenMissionList;
    for (int32_t Index = 0; Index < QuestDrivenMissionList.Num(); ++Index)
    {
        FFortQuestDrivenMission& QuestDrivenMission = QuestDrivenMissionList[Index];
        UFortQuestItemDefinition* RequiredQuest = QuestDrivenMission.RequiredQuest;
        bool bQuestActive = !RequiredQuest;
        for (int32_t PlayerIndex = 0; PlayerIndex < RegisteredPlayers.Num() && !bQuestActive; ++PlayerIndex)
        {
            UFortRegisteredPlayerInfo* PlayerInfo = RegisteredPlayers[PlayerIndex];
            UFortQuestManager* QuestManager = PlayerInfo ? static_cast<UFortQuestManager*>(PlayerInfo->QuestManager) : nullptr;
            bQuestActive = QuestManager && QuestManager->HasActiveQuest(RequiredQuest);
        }
        UFortMissionInfo* MissionInfo = bQuestActive ? AllowedGlobalMission(QuestDrivenMission) : nullptr;
        if (MissionInfo && !SharedMissions.Contains(MissionInfo))
        {
            UE_LOG(LogFortWorld, Log, TEXT("Adding quest-driven mission %s for %s"), *MissionInfo->GetName(), RequiredQuest ? *reinterpret_cast<UObject*>(RequiredQuest)->GetName() : TEXT("no quest"));
            SharedMissions.Add(MissionInfo);
        }
    }

    for (UFortMissionInfo* MissionInfo : SharedMissions)
    {
        UE_LOG(LogFortWorld, Log, TEXT("Adding shared mission %s (theater map data %s)"), *MissionInfo->GetName(), TheaterMapData ? TEXT("found") : TEXT("missing"));
    }
    if (SharedMissions.Num() > 0)
    {
        AddMissionsToMissionManagerRecord(SharedMissions);
    }
#endif
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

    FFortMissionManagerRecord& Record = MissionManagerRecord;
    TStructArray<FFortMissionRecord>& MissionRecords = Record.MissionRecords;
    for (int32_t Index = 0; Index < MissionRecords.Num(); ++Index)
    {
        FFortMissionEntry& MissionEntry = MissionRecords[Index].MissionEntry;
        UFortMissionInfo* MissionInfo = MissionEntry.MissionInfo;
        const EMissionGenerationCategory Category = MissionEntry.GenerationCategory;
        const FScriptArray& LookupData = MissionEntry.BlueprintLookupData;
        UE_LOG(LogFortWorld, Log, TEXT("Mission record %s (%s): %d placed actors and reserved locations"), MissionInfo ? *MissionInfo->GetName() : TEXT("None"), Category.ToString().c_str(), LookupData.Num());
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

bool AFortWorldManager::GetPlayerQuickBarRecords(AFortPlayerController* PlayerController, FQuickBar*& OutPrimary, FQuickBar*& OutSecondary)
{
#if WITH_SERVER_CODE
    UFortRegisteredPlayerInfo* PlayerInfo = PlayerController ? static_cast<UFortRegisteredPlayerInfo*>(PlayerController->MyPlayerInfo) : nullptr;
    if (UFortMcpProfileWorld* WorldProfile = PlayerInfo ? static_cast<UFortMcpProfileWorld*>(PlayerInfo->WorldProfile) : nullptr)
    {
        FFortWorldPlayerLoadout& Loadout = WorldProfile->CachedPlayerLoadout;
        OutPrimary = &static_cast<FQuickBar&>(Loadout.PrimaryQuickBarRecord);
        OutSecondary = &static_cast<FQuickBar&>(Loadout.SecondaryQuickBarRecord);
        return true;
    }

    AFortPlayerState* PlayerState = PlayerController ? Cast<AFortPlayerState>(reinterpret_cast<UObject*>(static_cast<APlayerState*>(PlayerController->PlayerState))) : nullptr;
    UFortWorldRecord* WorldRecord = CurrentWorldRecord;
    const int32_t WorldPlayerId = PlayerState ? static_cast<int32_t>(PlayerState->WorldPlayerId) : -1;
    if (!WorldRecord || WorldPlayerId < 0)
    {
        return false;
    }

    TStructArray<FFortPlayerRecord>& SavedPlayers = WorldRecord->SavedPlayers;
    if (WorldPlayerId >= SavedPlayers.Num())
    {
        return false;
    }

    FFortPlayerRecord& PlayerRecord = SavedPlayers[WorldPlayerId];
    OutPrimary = &static_cast<FQuickBar&>(PlayerRecord.PrimaryQuickBarRecord);
    OutSecondary = &static_cast<FQuickBar&>(PlayerRecord.SecondaryQuickBarRecord);
    return true;
#else
    return false;
#endif
}

bool AFortWorldManager::UpdatePlayerQuickBarRecords(AFortPlayerController* PlayerController)
{
#if WITH_SERVER_CODE
    AFortQuickBars* QuickBars = PlayerController ? static_cast<AFortQuickBars*>(PlayerController->QuickBars) : nullptr;
    FQuickBar* PrimaryRecord = nullptr;
    FQuickBar* SecondaryRecord = nullptr;
    if (!QuickBars || !GetPlayerQuickBarRecords(PlayerController, PrimaryRecord, SecondaryRecord))
    {
        return false;
    }

    UScriptStruct* QuickBarStruct = FQuickBar::StaticStruct();
    QuickBarStruct->CopyScriptStruct(PrimaryRecord, &QuickBars->GetQuickBar(EFortQuickBars::GetPrimary()));
    QuickBarStruct->CopyScriptStruct(SecondaryRecord, &QuickBars->GetQuickBar(EFortQuickBars::GetSecondary()));
#endif
    return true;
}

bool AFortWorldManager::RestorePlayerQuickBars(AFortPlayerController* PlayerController)
{
#if WITH_SERVER_CODE
    AFortQuickBars* QuickBars = PlayerController ? static_cast<AFortQuickBars*>(PlayerController->QuickBars) : nullptr;
    AFortInventory* Inventory = PlayerController ? static_cast<AFortInventory*>(PlayerController->WorldInventory) : nullptr;
    FQuickBar* Records[2] = {};
    if (!QuickBars || !Inventory || !GetPlayerQuickBarRecords(PlayerController, Records[0], Records[1]))
    {
        return false;
    }

    struct FSlottedItem
    {
        EFortQuickBars QuickBar;
        int32_t Slot;
        FGuid Item;
    };
    std::vector<FSlottedItem> SlottedItems;
    for (EFortQuickBars Bar : { EFortQuickBars::GetPrimary(), EFortQuickBars::GetSecondary() })
    {
        TStructArray<FQuickBarSlot>& Slots = Records[static_cast<int32_t>(Bar)]->Slots;
        for (int32_t SlotIndex = 0; SlotIndex < Slots.Num(); ++SlotIndex)
        {
            const TArray<FGuid>& Items = Slots[SlotIndex].Items;
            for (int32_t Index = 0; Index < Items.Num(); ++Index)
            {
                SlottedItems.push_back({ Bar, SlotIndex, Items[Index] });
            }
        }
    }

    for (const FSlottedItem& Slotted : SlottedItems)
    {
        UFortWorldItem* Item = Slotted.Slot < QuickBars->GetQuickBar(Slotted.QuickBar).Slots.Num() && !QuickBars->ContainsItem(Slotted.Item) ? Inventory->FindWorldItem(Slotted.Item) : nullptr;
        FFortItemEntry* ItemEntry = Item ? &static_cast<FFortItemEntry&>(Item->ItemEntry) : nullptr;
        UFortItemDefinition* ItemDefinition = ItemEntry ? static_cast<UFortItemDefinition*>(ItemEntry->ItemDefinition) : nullptr;
        if (ItemDefinition && QuickBars->GetSlotData(Slotted.QuickBar, Slotted.Slot).AcceptedItemTypes.Contains(ItemDefinition->ItemType))
        {
            QuickBars->ServerAddItemInternal(Slotted.Item, Slotted.QuickBar, Slotted.Slot);
        }
    }
    return true;
#else
    return false;
#endif
}

void AFortWorldManager::Hook()
{
    Hooks::Detour(L"AFortWorldManager::OnWorldRecordQueryComplete", reinterpret_cast<void*>(Natives::AFortWorldManager_OnWorldRecordQueryComplete), &OnWorldRecordQueryCompleteHK, &OnWorldRecordQueryCompleteOG);
    Hooks::Detour(L"AFortWorldManager::ServerTickLoadingOrCreatingWorld", reinterpret_cast<void*>(Natives::AFortWorldManager_ServerTickLoadingOrCreatingWorld), &ServerTickLoadingOrCreatingWorldHK, &ServerTickLoadingOrCreatingWorldOG);
}

void AFortWorldManager::HookCallSites()
{
    if (uint8_t* Call = Finder::QuickBarChangedWorldManagerCall())
    {
        Hooks::RedirectCall(L"AFortQuickBars quickbar changed -> AFortWorldManager::UpdatePlayerQuickBarRecords", Call, reinterpret_cast<void*>(&UpdatePlayerQuickBarRecordsCall));
    }
}

void AFortWorldManager::OnWorldRecordQueryCompleteHK(AFortWorldManager* This)
{
    This->OnWorldRecordQueryComplete();
}

void AFortWorldManager::ServerTickLoadingOrCreatingWorldHK(AFortWorldManager* This)
{
    This->ServerTickLoadingOrCreatingWorld();
}

bool AFortWorldManager::UpdatePlayerQuickBarRecordsCall(AFortWorldManager* This, AFortPlayerController* PlayerController)
{
    return This->UpdatePlayerQuickBarRecords(PlayerController);
}
