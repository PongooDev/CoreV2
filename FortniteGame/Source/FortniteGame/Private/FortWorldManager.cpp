#include "pch.h"
#include "FortWorldManager.h"
#include "Missions/FortMissionManager.h"
#include "BlueprintGameplayTagLibrary.h"
#include "Building/BuildingActor.h"
#include "Building/BuildingSMActor.h"
#include "Engine/Level.h"
#include "Engine/LevelStreaming.h"
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
#include "Log.h"
#include "Logging/LogMacros.h"
#include "Hooks.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Items/FortInventory.h"
#include "Items/FortItemDefinition.h"
#include "Items/FortQuickBars.h"
#include "Missions/FortMission.h"
#include "Missions/FortMissionLibrary.h"
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
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"

#include <algorithm>
#include <string>
#include <vector>

DEFINE_LOG_CATEGORY_STATIC(LogFortWorld, Log, All);
DEFINE_LOG_CATEGORY_STATIC(LogFortLootQuota, Log, All);

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

    // The AFortPlayerController virtual that sends the NewPlayer mission event once client and server have finished loading
    // (called from SetHasClientFinishedLoading); the other function with its error string only checks the player state and logs.
    uintptr_t FindNewPlayerEventSender()
    {
        uintptr_t Sender = 0;
        int32_t Matches = 0;
        for (const uintptr_t Ref : Finder::StringRefs(L"PlayerState Must subclass FortPlayerState!"))
        {
            const uintptr_t Function = Finder::FunctionStart(Ref);
            const uintptr_t End = Function ? Finder::FunctionEnd(Function) : 0;
            if (End && Function != Sender && Finder::DirectCalls(Function, End).size() > 4)
            {
                Sender = Function;
                ++Matches;
            }
        }
        return Matches == 1 ? Sender : 0;
    }

#if WITH_SERVER_CODE
    // A reflected member of Container (an object or struct of type Owner), or null when the build doesn't have it:
    // FFortZoneInstanceDetails gained its alert members over the builds (MissionAlertCategoryName from 1.7.2).
    const UProperty* FindMember(const UStruct* Owner, const wchar_t* Name, EClassCastFlags Kind)
    {
        const UProperty* Property = Owner ? Owner->FindPropertyByName(FName(Name, FNAME_Find)) : nullptr;
        return Property && Property->GetClass()->HasAnyCastFlag(Kind) ? Property : nullptr;
    }

    // The elements of a reflected TArray of structs Container.Name: the inner struct, the array, and its stride.
    struct FStructArrayView
    {
        const UScriptStruct* Struct = nullptr;
        const FScriptArray* Array = nullptr;
        int32_t Stride = 0;

        int32_t Num() const { return Array ? Array->Num() : 0; }
        const uint8_t* operator[](int32_t Index) const { return static_cast<const uint8_t*>(Array->GetData()) + static_cast<size_t>(Index) * Stride; }
    };

    FStructArrayView StructArrayMember(const UStruct* Owner, const void* Container, const wchar_t* Name)
    {
        const UArrayProperty* Property = static_cast<const UArrayProperty*>(FindMember(Owner, Name, CASTCLASS_UArrayProperty));
        const UProperty* Inner = Property && Container ? static_cast<const UProperty*>(Property->Inner) : nullptr;
        if (!Inner || !Inner->GetClass()->HasAnyCastFlag(CASTCLASS_UStructProperty))
        {
            return {};
        }
        return { static_cast<const UStructProperty*>(Inner)->Struct, Property->ContainerPtrToValuePtr<FScriptArray>(Container), Inner->ElementSize };
    }

    // Save the World mission alerts (Core-Backend #129). world/validate's reply is the world record's ZoneInstanceInfo
    // (FFortZoneInstanceDetails): ZoneModifiers (the mission's and the alert's GameplayModifier items), MissionAlertRewards
    // and MissionAlertCategoryName. The game applies them itself, on the path Core's zone flow takes:
    // - AFortWorldManager HandleWorldQueryComplete (1.9 0x658980, 1.9.1 0x658810; ticked once the record's query is
    //   complete) hands the details to the game state's vtable +0x860 (1.9 0x5CCE10, 1.9.1 0x5CCA80) whenever their
    //   ZoneThemeClass is set, which world/validate always echoes (the same call sets the zone theme and the mission
    //   generator Core's mission generation reads). That registers each ZoneModifiers item that is a
    //   UFortGameplayModifierItemDefinition (found by template id through the asset manager, once per quantity) into
    //   ActiveGameplayModifiers on the authority, and copies MissionAlertRewards' items into MissionAlertRewards.
    // - AFortGameMode::OnInitWorldRecordComplete (1.9 0x5BF350) looks MissionAlertCategoryName up in MissionAlertCategories
    //   and adds the row's CategoryTagsContainer (StormCategory/StormLowCategory: Stat.MissionAlert.Storm) to the game
    //   mode's GameContextTags, the context of every quest stat event (complete_stormzone*).
    // This only reports the result, once the zone starts loading (before missions, players and AI exist).
    void LogMissionAlert(AFortWorldManager* WorldManager)
    {
        UFortWorldRecord* Record = WorldManager->CurrentWorldRecord;
        const UStructProperty* DetailsProperty = Record ? static_cast<const UStructProperty*>(FindMember(UFortWorldRecord::StaticClass(), L"ZoneInstanceInfo", CASTCLASS_UStructProperty)) : nullptr;
        const UScriptStruct* DetailsStruct = DetailsProperty ? static_cast<UScriptStruct*>(DetailsProperty->Struct) : nullptr;
        if (!DetailsStruct)
        {
            return;
        }
        const void* Details = DetailsProperty->ContainerPtrToValuePtr<void>(Record);

        const UProperty* CategoryProperty = FindMember(DetailsStruct, L"MissionAlertCategoryName", CASTCLASS_UStrProperty);
        const std::wstring Category = CategoryProperty ? CategoryProperty->ContainerPtrToValuePtr<FString>(Details)->ToStdString() : std::wstring();

        // An FMcpLootResult member's Items (FMcpLootEntry: ItemType is the template id).
        const auto LootItems = [&](const wchar_t* Name) -> std::vector<std::pair<std::wstring, int32_t>>
        {
            std::vector<std::pair<std::wstring, int32_t>> Items;
            const UStructProperty* Result = static_cast<const UStructProperty*>(FindMember(DetailsStruct, Name, CASTCLASS_UStructProperty));
            const UScriptStruct* ResultStruct = Result ? static_cast<UScriptStruct*>(Result->Struct) : nullptr;
            const FStructArrayView Entries = StructArrayMember(ResultStruct, ResultStruct ? Result->ContainerPtrToValuePtr<void>(Details) : nullptr, L"Items");
            const UProperty* ItemType = FindMember(Entries.Struct, L"ItemType", CASTCLASS_UStrProperty);
            const UProperty* Quantity = FindMember(Entries.Struct, L"Quantity", CASTCLASS_UIntProperty);
            for (int32_t Index = 0; ItemType && Quantity && Index < Entries.Num(); ++Index)
            {
                Items.emplace_back(ItemType->ContainerPtrToValuePtr<FString>(Entries[Index])->ToStdString(), *Quantity->ContainerPtrToValuePtr<int32_t>(Entries[Index]));
            }
            return Items;
        };
        const auto Modifiers = LootItems(L"ZoneModifiers");
        const auto Rewards = LootItems(L"MissionAlertRewards");
        if (Category.empty() && Modifiers.empty() && Rewards.empty())
        {
            return;
        }

        int32_t ExpectedModifiers = 0;
        std::wstring Expected;
        for (const auto& [TemplateId, Quantity] : Modifiers)
        {
            ExpectedModifiers += _wcsnicmp(TemplateId.c_str(), L"GameplayModifier:", 17) == 0 ? Quantity : 0;
            Expected += (Expected.empty() ? L"" : L", ") + TemplateId + (Quantity != 1 ? std::format(L" x{}", Quantity) : L"");
        }

        UWorld* World = WorldManager->GetWorld();
        UObject* GameState = World ? reinterpret_cast<UObject*>(World->GetGameState()) : nullptr;
        const UClass* GameStateClass = GameState ? GameState->GetClass() : nullptr;
        const UStructProperty* ActiveProperty = static_cast<const UStructProperty*>(FindMember(GameStateClass, L"ActiveGameplayModifiers", CASTCLASS_UStructProperty));
        const UScriptStruct* ActiveStruct = ActiveProperty ? static_cast<UScriptStruct*>(ActiveProperty->Struct) : nullptr;
        const FStructArrayView Active = StructArrayMember(ActiveStruct, ActiveStruct ? ActiveProperty->ContainerPtrToValuePtr<void>(GameState) : nullptr, L"Items");
        const UProperty* ModifierDef = FindMember(Active.Struct, L"ModifierDef", CASTCLASS_UObjectPropertyBase);
        std::wstring Registered;
        for (int32_t Index = 0; ModifierDef && Index < Active.Num(); ++Index)
        {
            UObject* Definition = *ModifierDef->ContainerPtrToValuePtr<UObject*>(Active[Index]);
            Registered += (Registered.empty() ? L"" : L", ") + (Definition ? Definition->GetName().ToStdString() : std::wstring(L"None"));
        }

        const UArrayProperty* RewardsProperty = static_cast<const UArrayProperty*>(FindMember(GameStateClass, L"MissionAlertRewards", CASTCLASS_UArrayProperty));
        const int32_t GameStateRewards = RewardsProperty ? RewardsProperty->ContainerPtrToValuePtr<FScriptArray>(GameState)->Num() : -1;

        Log::Info(L"Mission alert: {}, modifiers: {} registered, rewards: {} (record: {} modifier items [{}], {} reward items)", Category.empty() ? L"none" : Category, Registered.empty() ? L"none" : Registered,
            GameStateRewards, Modifiers.size(), Expected.empty() ? L"none" : Expected, Rewards.size());
        if (Active.Num() < ExpectedModifiers || (GameStateRewards >= 0 && GameStateRewards < static_cast<int32_t>(Rewards.size())))
        {
            Log::Warning(L"Mission alert: the game state has {} of the record's {} gameplay modifiers and {} of its {} alert rewards", Active.Num(), ExpectedModifiers, GameStateRewards, Rewards.size());
        }
    }

    // The shared-mission rules of the surviving client check (AFortTheaterMapViewer::GetMissionQuestValidity -> 0x611AE0).
    struct FGlobalMissionFilter
    {
        UFortSharedMissionLists* SharedMissionLists = nullptr;
        FFortTheaterMapData* TheaterMapData = nullptr;
        TStructArray<FFortMissionRecord>* MissionRecords = nullptr;
        const FGameplayTagContainer* GameContextTags = nullptr;
        UFortZoneTheme* ZoneTheme = nullptr;
        FGameplayTagContainer PrimaryMissionTags;
        EFortTheaterType TheaterType = EFortTheaterType::GetMax_None();
        float GameDifficulty = 0.0f;
        bool bIsTestMap = false;

        explicit FGlobalMissionFilter(AFortWorldManager* WorldManager)
        {
            UWorld* World = WorldManager->GetWorld();
            AFortGameMode* GameMode = World ? Cast<AFortGameMode>(reinterpret_cast<UObject*>(World->GetAuthGameMode())) : nullptr;
            SharedMissionLists = GameMode ? GameMode->GetSharedMissionLists() : nullptr;
            if (!SharedMissionLists)
            {
                return;
            }

            AFortGameStateZone* GameState = Cast<AFortGameStateZone>(reinterpret_cast<UObject*>(World->GetGameState()));
            GameDifficulty = GameState ? static_cast<float>(GameState->GameDifficulty) : 0.0f;
            ZoneTheme = GameState ? static_cast<UFortZoneTheme*>(GameState->ZoneTheme) : nullptr;
            GameContextTags = &static_cast<const FGameplayTagContainer&>(GameMode->GameContextTags);

            if (!Natives::AFortWorldManager_GetTheaterMapData)
            {
                Finder::ResolveTheaterMapDataLive();
            }
            TheaterMapData = Natives::AFortWorldManager_GetTheaterMapData ? static_cast<FFortTheaterMapData*>(Natives::AFortWorldManager_GetTheaterMapData(WorldManager)) : nullptr;
            TheaterType = TheaterMapData ? TheaterMapData->RuntimeInfo.TheaterType : EFortTheaterType::GetMax_None();
            bIsTestMap = TheaterType == EFortTheaterType::GetTest() || TheaterType == EFortTheaterType::GetMax_None();

            FFortMissionManagerRecord& Record = WorldManager->MissionManagerRecord;
            MissionRecords = &static_cast<TStructArray<FFortMissionRecord>&>(Record.MissionRecords);
            for (int32_t Index = 0; Index < MissionRecords->Num(); ++Index)
            {
                FFortMissionEntry& MissionEntry = (*MissionRecords)[Index].MissionEntry;
                UFortMissionInfo* MissionInfo = MissionEntry.MissionInfo;
                if (MissionInfo && MissionInfo->MissionType == EFortMissionType::GetPrimary())
                {
                    UBlueprintGameplayTagLibrary::AppendGameplayTagContainers(PrimaryMissionTags, MissionInfo->MissionTags);
                }
            }
        }

        const wchar_t* Reject(FFortGlobalMission& SharedMission) const
        {
            if (!MatchesQuery(PrimaryMissionTags, SharedMission.PrimaryMissionTagQuery))
            {
                return L"primary mission tag query";
            }

            const float MinDifficultyLevel = SharedMission.MinDifficultyLevel;
            const float MaxDifficultyLevel = SharedMission.MaxDifficultyLevel;
            if ((MinDifficultyLevel >= 0.0f && GameDifficulty < MinDifficultyLevel) || (MaxDifficultyLevel >= 0.0f && GameDifficulty > MaxDifficultyLevel))
            {
                return L"difficulty";
            }

            if ((SharedMission.bIsPrototype && TheaterType != EFortTheaterType::GetTest()) || (bIsTestMap && !SharedMission.bAllowInTestMaps))
            {
                return L"prototype or test map";
            }

            if (TheaterMapData)
            {
                const TArray<EFortTheaterType>& AllowedTheaterTypes = SharedMission.AllowedTheaterTypes;
                if ((AllowedTheaterTypes.Num() > 0 && !AllowedTheaterTypes.Contains(TheaterType)) || !MatchesQuery(TheaterMapData->RuntimeInfo.TheaterTags, SharedMission.TheaterTagQuery))
                {
                    return L"theater";
                }
            }

            if (ZoneTheme)
            {
                const TArray<EFortZoneType>& AllowedZoneTypes = SharedMission.AllowedZoneTypes;
                if ((AllowedZoneTypes.Num() > 0 && !AllowedZoneTypes.Contains(ZoneTheme->ZoneType)) || !MatchesQuery(ZoneTheme->ZoneTags, SharedMission.ZoneTagQuery))
                {
                    return L"zone type or tags";
                }
            }

            if (!MatchesQuery(*GameContextTags, SharedMission.RegionTagQuery))
            {
                return L"region tag query";
            }
            return nullptr;
        }

        std::wstring ContextTags() const
        {
            std::wstring Text;
            for (int32_t Index = 0; GameContextTags && Index < GameContextTags->GameplayTags.Num(); ++Index)
            {
                Text += (Text.empty() ? L"" : L", ") + GameContextTags->GameplayTags[Index].TagName.ToStdString();
            }
            return Text.empty() ? std::wstring(L"none") : Text;
        }

        bool IsGenerated(UFortMissionInfo* MissionInfo) const
        {
            for (int32_t Index = 0; MissionRecords && Index < MissionRecords->Num(); ++Index)
            {
                FFortMissionEntry& MissionEntry = (*MissionRecords)[Index].MissionEntry;
                if (static_cast<UFortMissionInfo*>(MissionEntry.MissionInfo) == MissionInfo)
                {
                    return true;
                }
            }
            return false;
        }
    };
#endif
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
            LogMissionAlert(this);

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
    FGlobalMissionFilter Filter(this);
    UFortSharedMissionLists* SharedMissionLists = Filter.SharedMissionLists;
    if (!SharedMissionLists)
    {
        return;
    }

    auto AllowedGlobalMission = [&](FFortGlobalMission& SharedMission) -> UFortMissionInfo*
    {
        if (Filter.Reject(SharedMission))
        {
            return nullptr;
        }

        const TAssetPtr<UFortMissionInfo>& SharedMissionInfo = SharedMission.MissionInfo;
        UFortMissionInfo* MissionInfo = SharedMissionInfo.LoadSynchronous();
        return MissionInfo && !Filter.IsGenerated(MissionInfo) ? MissionInfo : nullptr;
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

    UWorld* World = GetWorld();
    UFortGameInstance* GameInstance = Cast<UFortGameInstance>(reinterpret_cast<UObject*>(World->OwningGameInstance));
    TArray<UFortRegisteredPlayerInfo*> RegisteredPlayers;
    if (GameInstance)
    {
        RegisteredPlayers = GameInstance->RegisteredPlayers;
    }
    TStructArray<FFortQuestDrivenMission>& QuestDrivenMissionList = SharedMissionLists->QuestDrivenMissionList;
    UE_LOG(LogFortWorld, Log, TEXT("Checking %d quest-driven missions against %d registered players"), QuestDrivenMissionList.Num(), RegisteredPlayers.Num());
    for (int32_t Index = 0; Index < QuestDrivenMissionList.Num(); ++Index)
    {
        FFortQuestDrivenMission& QuestDrivenMission = QuestDrivenMissionList[Index];
        UFortQuestItemDefinition* RequiredQuest = QuestDrivenMission.RequiredQuest;
        bool bQuestActive = !RequiredQuest;
        for (int32_t PlayerIndex = 0; PlayerIndex < RegisteredPlayers.Num() && !bQuestActive; ++PlayerIndex)
        {
            UFortRegisteredPlayerInfo* PlayerInfo = RegisteredPlayers[PlayerIndex];
            UFortQuestManager* QuestManager = PlayerInfo ? PlayerInfo->GetCampaignQuestManager() : nullptr;
            bQuestActive = QuestManager && QuestManager->HasActiveQuest(RequiredQuest);
        }
        if (const wchar_t* Reason = bQuestActive && RequiredQuest ? Filter.Reject(QuestDrivenMission) : nullptr)
        {
            UE_LOG(LogFortWorld, Log, TEXT("Quest-driven mission for %s is not allowed in this zone (%s; zone context tags %s)"), *reinterpret_cast<UObject*>(RequiredQuest)->GetName(), Reason, Filter.ContextTags().c_str());
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
        UE_LOG(LogFortWorld, Log, TEXT("Adding shared mission %s (theater map data %s)"), *MissionInfo->GetName(), Filter.TheaterMapData ? TEXT("found") : TEXT("missing"));
    }
    if (SharedMissions.Num() > 0)
    {
        AddMissionsToMissionManagerRecord(SharedMissions);
    }
#endif
}

// Players whose quest needs a quest-driven mission the zone does not run yet (joined after the world was created, or their
// quests were not loaded then) get it before the mission manager sends their NewPlayer event, so its blueprint sees them.
void AFortWorldManager::LoadQuestDrivenMissionsForPlayer(AFortPlayerController* PlayerController)
{
#if WITH_SERVER_CODE
    UWorld* World = GetWorld();
    AFortGameState* GameState = World ? Cast<AFortGameState>(reinterpret_cast<UObject*>(World->GetGameState())) : nullptr;
    AFortMissionManager* MissionManager = GameState ? static_cast<AFortMissionManager*>(GameState->MissionManager) : nullptr;
    UFortQuestManager* QuestManager = PlayerController ? PlayerController->GetQuestManager() : nullptr;
    if (!bHasCreatedMissions || !MissionManager || !QuestManager)
    {
        return;
    }

    FGlobalMissionFilter Filter(this);
    UFortSharedMissionLists* SharedMissionLists = Filter.SharedMissionLists;
    TStructArray<FFortQuestDrivenMission>* QuestDrivenMissionList = SharedMissionLists ? &static_cast<TStructArray<FFortQuestDrivenMission>&>(SharedMissionLists->QuestDrivenMissionList) : nullptr;
    for (int32_t Index = 0; QuestDrivenMissionList && Index < QuestDrivenMissionList->Num(); ++Index)
    {
        FFortQuestDrivenMission& QuestDrivenMission = (*QuestDrivenMissionList)[Index];
        UFortQuestItemDefinition* RequiredQuest = QuestDrivenMission.RequiredQuest;
        if (!RequiredQuest || !QuestManager->HasActiveQuest(RequiredQuest))
        {
            continue;
        }

        if (const wchar_t* Reason = Filter.Reject(QuestDrivenMission))
        {
            UE_LOG(LogFortWorld, Log, TEXT("Quest-driven mission for %s is not allowed in this zone (%s; zone context tags %s)"), *reinterpret_cast<UObject*>(RequiredQuest)->GetName(), Reason, Filter.ContextTags().c_str());
            continue;
        }

        const TAssetPtr<UFortMissionInfo>& QuestMissionInfo = QuestDrivenMission.MissionInfo;
        UFortMissionInfo* MissionInfo = QuestMissionInfo.LoadSynchronous();
        const TArray<AFortMission*>& Missions = MissionManager->Missions;
        bool bLoaded = !MissionInfo;
        for (int32_t MissionIndex = 0; MissionIndex < Missions.Num() && !bLoaded; ++MissionIndex)
        {
            AFortMission* Mission = Missions[MissionIndex];
            bLoaded = Mission && static_cast<UFortMissionInfo*>(Mission->MissionInfo) == MissionInfo;
        }
        if (!bLoaded)
        {
            UE_LOG(LogFortWorld, Log, TEXT("Loading quest-driven mission %s for %s (%s joined)"), *MissionInfo->GetName(), *reinterpret_cast<UObject*>(RequiredQuest)->GetName(), *PlayerController->GetName());
            UFortMissionLibrary::LoadMission(this, MissionInfo);
        }
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

FFortLootQuotaData& FFortTileLootData::GetLootQuota(uint8_t QuotaLevel) const
{
    uint8_t* LootQuotas = reinterpret_cast<uint8_t*>(&GetMember_LootQuotas());
    return *reinterpret_cast<FFortLootQuotaData*>(LootQuotas + static_cast<size_t>(QuotaLevel) * FFortLootQuotaData::StaticStruct()->GetStructureSize());
}

int32_t& FFortTileLootData::GetLootDrops(uint8_t QuotaLevel) const
{
    return (&GetMember_LootDrops())[QuotaLevel];
}

int32_t AFortWorldManager::GetWorldLevel() const
{
    UWorld* World = GetWorld();
    AFortGameState* GameState = World ? Cast<AFortGameState>(reinterpret_cast<UObject*>(World->GetGameState())) : nullptr;
    return GameState ? static_cast<int32_t>(GameState->WorldLevel) : 0;
}

FFortBuildingRandomList* AFortWorldManager::GetLootRandomList()
{
    const ptrdiff_t Offset = Natives::AFortWorldManager_LootRandomList;
    return Offset > 0 ? reinterpret_cast<FFortBuildingRandomList*>(reinterpret_cast<uint8_t*>(this) + Offset) : nullptr;
}

void AFortWorldManager::SpawnTileLoot(FFortTileLootData* TileLootData, const TArray<int32_t>& LevelIndices)
{
#if WITH_SERVER_CODE
    struct FQuotaBuilding
    {
        ABuildingSMActor* Building = nullptr;
        std::vector<FFortLootTierRow> LootTierRows;
    };

    UWorld* World = GetWorld();
    const bool bSpawnAllLoot = FParse::Param(FCommandLine::Get(), L"SpawnAllLoot");
    UFortGameData* GameData = UFortAssetManager::Get().GetGameData();
    if (!TileManager || !GameData || !GameData->HasLootTierDataTables() || !TileLootData || bSpawnAllLoot || !World || WorldManagerState != EFortWorldManagerState::GetWMS_CreatingNewWorld())
    {
        return;
    }

    const uint8_t UnlimitedQuota = ELootQuotaLevel::GetUnlimited();
    const uint8_t NumQuotaLevels = ELootQuotaLevel::GetNumLevels();
    std::vector<std::vector<FQuotaBuilding>> Buildings(NumQuotaLevels);
    TStructArray<FFortLevelStreamingInfo>& Levels = StreamedLevels;
    for (const int32_t LevelIndex : LevelIndices)
    {
        if (LevelIndex < 0 || LevelIndex >= Levels.Num())
        {
            continue;
        }
        ULevelStreaming* LevelStreaming = World->GetLevelStreamingForPackageName(Levels[LevelIndex].PackageName);
        ULevel* Level = LevelStreaming ? static_cast<ULevel*>(LevelStreaming->LoadedLevel) : LevelIndex == 0 ? static_cast<ULevel*>(World->PersistentLevel) : nullptr;
        if (!Level)
        {
            continue;
        }
        for (AActor* Actor : Level->GetActors())
        {
            ABuildingSMActor* Building = Actor ? Cast<ABuildingSMActor>(reinterpret_cast<UObject*>(Actor)) : nullptr;
            if (!Building)
            {
                continue;
            }
            uint8_t QuotaLevel = Building->GetMaximumQuotaLevelBound();
            if (QuotaLevel != UnlimitedQuota && QuotaLevel <= NumQuotaLevels)
            {
                Buildings[QuotaLevel == NumQuotaLevels ? NumQuotaLevels - 1 : QuotaLevel].push_back({ Building, {} });
            }
        }
    }

    const int32_t WorldLevel = GetWorldLevel();
    FFortBuildingRandomList* RandomList = GetLootRandomList();
    for (int32_t QuotaLevel = NumQuotaLevels - 1; QuotaLevel >= 1; --QuotaLevel)
    {
        std::vector<FQuotaBuilding>& Candidates = Buildings[QuotaLevel];
        int32_t& LootDrops = TileLootData->GetLootDrops(static_cast<uint8_t>(QuotaLevel));
        while (LootDrops > 0 && !Candidates.empty())
        {
            const float Roll = RandomList ? RandomList->Next() : 0.0f;
            const int32_t Index = static_cast<int32_t>((static_cast<float>(Candidates.size()) - 0.0000099999997f) * Roll);
            FQuotaBuilding& Candidate = Candidates[Index];
            ABuildingSMActor* Building = Candidate.Building;
            if (Candidate.LootTierRows.empty())
            {
                float TotalWeight = 0.0f;
                UFortLootTier::GetLootTierRows(Candidate.LootTierRows, TotalWeight, Building->GetQuotaLootTierGroup(), WorldLevel, -1, Building->StaticGameplayTags, NumQuotaLevels);
            }

            FName LootTierKey;
            int32_t LootTier = 0;
            if (Candidate.LootTierRows.empty())
            {
                Building->OnQuotaNotChosen();
            }
            else if (UFortLootTier::PickLootTierForQuotaLevel(LootTierKey, LootTier, Candidate.LootTierRows, static_cast<uint8_t>(QuotaLevel), nullptr))
            {
                --LootDrops;
                Building->SetChosenQuotaInfo(LootTierKey, LootTier);
            }
            else if (Building->CanSpawnAtLowerQuotaLevels())
            {
                uint8_t LowerQuotaLevel = 0;
                for (const FFortLootTierRow& Row : Candidate.LootTierRows)
                {
                    const uint8_t RowQuotaLevel = Row.LootTierData->QuotaLevel;
                    if (RowQuotaLevel > LowerQuotaLevel && RowQuotaLevel < QuotaLevel)
                    {
                        LowerQuotaLevel = RowQuotaLevel;
                    }
                }
                if (LowerQuotaLevel)
                {
                    Buildings[LowerQuotaLevel].push_back({ Building, Candidate.LootTierRows });
                }
            }
            else
            {
                Building->OnQuotaNotChosen();
            }

            if (Index != static_cast<int32_t>(Candidates.size()) - 1)
            {
                Candidates[Index] = std::move(Candidates.back());
            }
            Candidates.pop_back();
        }

        if (Candidates.empty() && LootDrops > 0)
        {
            UE_LOG(LogFortLootQuota, Log, TEXT("Attempted to give a tile more loot drops than it has valid locations for. Check quota max."));
        }
        for (FQuotaBuilding& Remaining : Candidates)
        {
            if (!Remaining.Building->CanSpawnAtLowerQuotaLevels())
            {
                Remaining.Building->OnQuotaNotChosen();
            }
            else if (QuotaLevel - 1 > 0)
            {
                Buildings[QuotaLevel - 1].push_back(Remaining);
            }
        }
    }
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

    if (const uintptr_t NewPlayerEventSender = FindNewPlayerEventSender())
    {
        Hooks::Detour(L"AFortPlayerController NewPlayer mission event", reinterpret_cast<void*>(NewPlayerEventSender), &SendNewPlayerEventHK, &SendNewPlayerEventOG);
    }
    else
    {
        Log::Error(L"Finder: AFortPlayerController NewPlayer mission event sender not found");
    }
}

void AFortWorldManager::HookCallSites()
{
    if (uint8_t* Call = Finder::QuickBarChangedWorldManagerCall())
    {
        Hooks::RedirectCall(L"AFortQuickBars quickbar changed -> AFortWorldManager::UpdatePlayerQuickBarRecords", Call, reinterpret_cast<void*>(&UpdatePlayerQuickBarRecordsCall));
    }

    Finder::FConditionalFoundationQuotaSites QuotaSites;
    if (Finder::ResolveConditionalFoundationQuotaLive(UFortGameData::ConditionalFoundationQuotaOffset(), QuotaSites))
    {
        Natives::AFortWorldManager_ConditionalFoundationRequestSize = QuotaSites.RequestSize;
        Natives::AFortWorldManager_ConditionalFoundationRequestClasses = QuotaSites.RequestClassesOffset;
        Natives::AFortWorldManager_ConditionalFoundationRequestsArray = QuotaSites.RequestsArrayOffset;
        Natives::AFortWorldManager_ConditionalFoundationQuota = reinterpret_cast<decltype(Natives::AFortWorldManager_ConditionalFoundationQuota)>(QuotaSites.Quota);
        Natives::AFortWorldManager_ConditionalFoundationTierIndex = QuotaSites.TierIndexOffset;
        Hooks::RedirectCall(L"AFortWorldManager conditional foundation quota context", QuotaSites.ContextCall, reinterpret_cast<void*>(&ConditionalFoundationContextCall));
        Hooks::RedirectCall(L"AFortWorldManager conditional foundation quota selection", QuotaSites.SelectCall, reinterpret_cast<void*>(&SelectConditionalFoundationsCall));
        if (QuotaSites.QuotaCall)
        {
            Hooks::RedirectCall(L"AFortWorldManager conditional foundation quota pass", QuotaSites.QuotaCall, reinterpret_cast<void*>(&ConditionalFoundationQuotaCall));
        }
    }

    HookWorldLoot();

    // The offsets resolve in their own statements: the arguments are evaluated right to left.
    const ptrdiff_t DistanceRangeMinOffset = Member_ExplorationActorDistanceRangeMin.GetOffsetChecked();
    const ptrdiff_t DistanceRangeMaxOffset = Member_ExplorationActorDistanceRangeMax.GetOffsetChecked();
    if (uint8_t* Call = Finder::ExplorationActorFinderCall(DistanceRangeMinOffset, DistanceRangeMaxOffset))
    {
        Hooks::RedirectCall(L"AFortWorldManager::SpawnExplorationActors placement finder", Call, reinterpret_cast<void*>(&SpawnExplorationActorsCall));
    }
}

void AFortWorldManager::HookWorldLoot()
{
    // The mask is only set once the offset resolves, and the arguments are evaluated right to left.
    const ptrdiff_t ConstructTileWorldOffset = Member_bConstructTileWorld.GetOffsetChecked();
    Finder::FWorldLootSites LootSites;
    if (!Finder::ResolveWorldLootLive(Member_DefaultLootQuotaCategory.GetOffsetChecked(), Member_LootData.GetOffsetChecked(), ConstructTileWorldOffset, Member_bConstructTileWorld.GetFieldMask(), LootSites))
    {
        return;
    }

    // 1.2: the quota fill the zone and tile setups call is the shared empty function.
    bool bTileQuotasFilled = true;
    if (Finder::IsStubFunction(LootSites.Quota))
    {
        Hooks::RedirectCall(L"FFortLootQuotaData::GetLootQuotasForCategory (zone)", LootSites.ZoneQuotaCall, reinterpret_cast<void*>(&GetLootQuotasForCategoryCall));
        bTileQuotasFilled = LootSites.TileQuotaCall && Hooks::RedirectCall(L"FFortLootQuotaData::GetLootQuotasForCategory (tiles)", LootSites.TileQuotaCall, reinterpret_cast<void*>(&GetLootQuotasForCategoryCall));
        if (!bTileQuotasFilled)
        {
            Log::Error(L"Finder: the tile loot quota call was not found; the tile loot distribution stays off");
        }
    }

    // 1.2: the tick still fetches each new tile's loot record but no longer hands its quota-bound buildings their tiers.
    // Without the tiles' quotas every quota-bound building would be hidden, so it needs the tile fill too.
    if (!LootSites.bSpawnsTileLoot && bTileQuotasFilled && LootSites.TileRecordCall && ABuildingSMActor::ResolveLootQuota())
    {
        Hooks::RedirectCall(L"AFortWorldManager::SpawnTileLoot", LootSites.TileRecordCall, reinterpret_cast<void*>(&GetTileRecordCall));
    }
}

void AFortWorldManager::GetLootQuotasForCategoryCall(const FName* QuotaCategory, FFortTileLootData* OutLootData, int32_t WorldLevel)
{
    FFortLootQuotaData::GetLootQuotasForCategory(*QuotaCategory, *OutLootData, WorldLevel);
}

FFortTileLootData* AFortWorldManager::GetTileRecordCall(AFortWorldManager* This, void* TileInfo, int32_t TileIndex, TArray<int32_t>& OutLevelIndices)
{
    FFortTileLootData* TileLootData = static_cast<FFortTileLootData*>(Natives::AFortWorldManager_GetTileRecord(This, TileInfo, TileIndex, &OutLevelIndices));
    if (TileLootData)
    {
        This->SpawnTileLoot(TileLootData, OutLevelIndices);
    }
    return TileLootData;
}

void AFortWorldManager::OnWorldRecordQueryCompleteHK(AFortWorldManager* This)
{
    This->OnWorldRecordQueryComplete();
}

void AFortWorldManager::ServerTickLoadingOrCreatingWorldHK(AFortWorldManager* This)
{
    This->ServerTickLoadingOrCreatingWorld();
}

void AFortWorldManager::SendNewPlayerEventHK(AFortPlayerController* PlayerController)
{
#if WITH_SERVER_CODE
    UWorld* World = PlayerController && PlayerController->Role == ROLE_Authority ? PlayerController->GetWorld() : nullptr;
    AFortGameState* GameState = World ? Cast<AFortGameState>(reinterpret_cast<UObject*>(World->GetGameState())) : nullptr;
    if (AFortWorldManager* WorldManager = GameState ? static_cast<AFortWorldManager*>(GameState->WorldManager) : nullptr)
    {
        WorldManager->LoadQuestDrivenMissionsForPlayer(PlayerController);
    }
#endif
    SendNewPlayerEventOG(PlayerController);
}

bool AFortWorldManager::UpdatePlayerQuickBarRecordsCall(AFortWorldManager* This, AFortPlayerController* PlayerController)
{
    return This->UpdatePlayerQuickBarRecords(PlayerController);
}

int32_t AFortWorldManager::GetConditionalFoundationTierIndex() const
{
    const ptrdiff_t Offset = Natives::AFortWorldManager_ConditionalFoundationTierIndex;
    return Offset > 0 ? *reinterpret_cast<const int32_t*>(reinterpret_cast<const uint8_t*>(this) + Offset) : -1;
}

void AFortWorldManager::ConditionalFoundationQuotaCall(AFortWorldManager* This)
{
#if WITH_SERVER_CODE
    static const AFortWorldManager* LastWorldManager = nullptr;
    static uint8_t LastState = 0xFF;
    static int32_t LastTier = -2;
    const uint8_t State = This ? static_cast<uint8_t>(This->WorldManagerState) : 0xFF;
    const int32_t Tier = This ? This->GetConditionalFoundationTierIndex() : -1;
    if (This && (This != LastWorldManager || State != LastState || Tier != LastTier))
    {
        LastWorldManager = This;
        LastState = State;
        LastTier = Tier;
        UFortGameData* GameData = UFortAssetManager::Get().GetGameData();
        const FScriptArray* Tiers = GameData ? reinterpret_cast<const FScriptArray*>(reinterpret_cast<const uint8_t*>(GameData) + UFortGameData::ConditionalFoundationQuotaOffset()) : nullptr;
        const EFortWorldManagerState WorldState = This->WorldManagerState;
        UE_LOG(LogFortWorld, Log, TEXT("Conditional foundation quota pass: %s, tier %d of %d"), WorldState.ToString().c_str(), Tier, Tiers ? Tiers->Num() : -1);
    }
#endif
    Natives::AFortWorldManager_ConditionalFoundationQuota(This);
}

AFortWorldManager* AFortWorldManager::ConditionalFoundationContextCall(AFortWorldManager* This)
{
#if WITH_SERVER_CODE
    UE_LOG(LogFortWorld, Log, TEXT("Conditional foundation quota: tier %d context"), This ? This->GetConditionalFoundationTierIndex() : -1);
    return This;
#else
    return nullptr;
#endif
}

bool AFortWorldManager::SelectConditionalFoundationsCall(AFortWorldManager* This, const void* Query, TArray<AActor*>& OutSelected, void*, void* RandomStream, float, bool)
{
    const ptrdiff_t RequestsOffset = Natives::AFortWorldManager_ConditionalFoundationRequestsArray;
    return This && Query && RequestsOffset >= 0 && This->SelectConditionalFoundations(*reinterpret_cast<const FScriptArray*>(static_cast<const uint8_t*>(Query) + RequestsOffset), OutSelected, RandomStream);
}

bool AFortWorldManager::SelectConditionalFoundations(const FScriptArray& Requests, TArray<AActor*>& OutSelected, void* RandomStream)
{
#if WITH_SERVER_CODE
    const int32_t RequestSize = Natives::AFortWorldManager_ConditionalFoundationRequestSize;
    const ptrdiff_t ClassesOffset = Natives::AFortWorldManager_ConditionalFoundationRequestClasses;
    UWorld* World = GetWorld();
    UE_LOG(LogFortWorld, Log, TEXT("Conditional foundation quota: %d requests for tier %d"), Requests.Num(), GetConditionalFoundationTierIndex());
    if (!World || RequestSize <= 0 || ClassesOffset < 0)
    {
        return false;
    }

    for (int32_t Index = 0; Index < Requests.Num(); ++Index)
    {
        const TArray<UClass*>& Classes = *reinterpret_cast<const TArray<UClass*>*>(static_cast<const uint8_t*>(Requests.GetData()) + static_cast<size_t>(Index) * RequestSize + ClassesOffset);
        std::vector<AActor*> Candidates;
        for (TObjectIterator<AActor> It; It; ++It)
        {
            AActor* Actor = *It;
            if (Actor->GetWorld() == World && !OutSelected.Contains(Actor) && std::any_of(Classes.begin(), Classes.end(), [Actor](UClass* Class) { return Class && Actor->IsA(Class); }))
            {
                Candidates.push_back(Actor);
            }
        }
        if (Candidates.empty())
        {
            UE_LOG(LogFortWorld, Warning, TEXT("Conditional foundation quota: no foundation left for request %d of %d"), Index + 1, Requests.Num());
            break;
        }

        const int32_t NumCandidates = static_cast<int32_t>(Candidates.size());
        AActor* Selected = Candidates[std::min(static_cast<int32_t>(RandomStreamFraction(RandomStream) * NumCandidates), NumCandidates - 1)];
        OutSelected.Add(Selected);
        UE_LOG(LogFortWorld, Log, TEXT("Conditional foundation quota selects %s (%d candidates)"), *Selected->GetName(), NumCandidates);
    }
    return OutSelected.Num() == Requests.Num();
#else
    return false;
#endif
}

// AFortWorldManager::SpawnExplorationActors (world initialization when the game mode's bSpawnExplorationActorsAtWorldInitialization
// is set, an outpost's storm shield defense, the RespawnOutlanderFragments mission event) asks for NumExplorationActors (16) of
// ExplorationActorClass, the Outlander fragment container B_Fragment_Container, at placement actors with
// ExplorationActorRequiredTags (PlacementActor.EnemySpawner, the tiles' DormantSpawnerPlacementActors) with a 6000 to 60000 uu
// distance range, and keeps what comes back in SpawnedExplorationActors. The client build strips the finder to an empty array, so no fragment machine ever spawned.
// The finder's rules are a guess: random matching placement actors not used yet, each new one within the distance range of
// the nearest spot already chosen (the one farthest from the others when none is), spawned at the placement actor.
TArray<AActor*>* AFortWorldManager::SpawnExplorationActorsCall(TArray<AActor*>* Result, UWorld* World, const FFortPlacementActorFinderInfo* FinderInfo, bool* bOutSucceeded, void* RandomStream)
{
    new (Result) TArray<AActor*>();
#if WITH_SERVER_CODE
    TStructArray<FFortPlacementActorFinderEntry>* Entries = FinderInfo ? &static_cast<TStructArray<FFortPlacementActorFinderEntry>&>(FinderInfo->BuildingData) : nullptr;
    if (!World || !Entries)
    {
        return Result;
    }

    const float MinDistance = FinderInfo->DistanceRangeMin;
    const float MaxDistance = FinderInfo->DistanceRangeMax;
    std::vector<ABuildingActor*> Used;
    std::vector<FVector> Chosen;
    int32_t NumCandidates = 0;
    UClass* SpawnedClass = nullptr;
    for (int32_t Index = 0; Index < Entries->Num(); ++Index)
    {
        FFortPlacementActorFinderEntry& Entry = (*Entries)[Index];
        UClass* BuildingToSpawn = Entry.BuildingToSpawn.LoadSynchronous();
        if (!BuildingToSpawn)
        {
            continue;
        }

        const TArray<UClass*>& ClassesToFind = Entry.BuildingClassesToFind;
        const FGameplayTagContainer& RequiredTags = Entry.RequiredTags;
        const FGameplayTagContainer& PreferredTags = Entry.PreferredTags;
        const FGameplayTagContainer& ExclusionTags = Entry.ExlusionTags;
        const auto HasTag = [](ABuildingActor* Building, const FGameplayTag& Tag)
        {
            const FGameplayTagContainer& StaticTags = Building->StaticGameplayTags;
            const FGameplayTagContainer& DefaultTags = Building->DefaultTags;
            return StaticTags.HasTag(Tag) || DefaultTags.HasTag(Tag);
        };

        std::vector<ABuildingActor*> Candidates;
        std::vector<ABuildingActor*> Preferred;
        for (TObjectIterator<ABuildingActor> It; It; ++It)
        {
            ABuildingActor* Building = *It;
            const bool bClass = ClassesToFind.Num() > 0 ? std::any_of(ClassesToFind.begin(), ClassesToFind.end(), [Building](UClass* Class) { return Class && Building->IsA(Class); }) : Building->IsA(AFortPlacementActor::StaticClass());
            if (!bClass || Building->GetWorld() != World || Building->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject) || std::find(Used.begin(), Used.end(), Building) != Used.end()
                || !std::all_of(RequiredTags.GameplayTags.begin(), RequiredTags.GameplayTags.end(), [&](const FGameplayTag& Tag) { return HasTag(Building, Tag); })
                || std::any_of(ExclusionTags.GameplayTags.begin(), ExclusionTags.GameplayTags.end(), [&](const FGameplayTag& Tag) { return HasTag(Building, Tag); }))
            {
                continue;
            }
            Candidates.push_back(Building);
            if (PreferredTags.Num() > 0 && std::all_of(PreferredTags.GameplayTags.begin(), PreferredTags.GameplayTags.end(), [&](const FGameplayTag& Tag) { return HasTag(Building, Tag); }))
            {
                Preferred.push_back(Building);
            }
        }
        NumCandidates = std::max(NumCandidates, static_cast<int32_t>(Candidates.size()));
        std::vector<ABuildingActor*>& Pool = Preferred.empty() ? Candidates : Preferred;
        if (Pool.empty())
        {
            continue;
        }

        const auto NearestChosen = [&Chosen](ABuildingActor* Building)
        {
            const FVector Location = Building->GetTransform().GetLocation();
            float Nearest = -1.0f;
            for (const FVector& Other : Chosen)
            {
                const float Distance = FMath::Sqrt(FVector::DistSquared(Location, Other));
                Nearest = Nearest < 0.0f || Distance < Nearest ? Distance : Nearest;
            }
            return Nearest;
        };
        std::vector<ABuildingActor*> InRange;
        for (ABuildingActor* Building : Pool)
        {
            const float Nearest = NearestChosen(Building);
            if (Nearest < 0.0f || (Nearest >= MinDistance && (MaxDistance <= 0.0f || Nearest <= MaxDistance)))
            {
                InRange.push_back(Building);
            }
        }

        ABuildingActor* Spot = nullptr;
        if (!InRange.empty())
        {
            const int32_t NumInRange = static_cast<int32_t>(InRange.size());
            Spot = InRange[std::min(static_cast<int32_t>(RandomStreamFraction(RandomStream) * NumInRange), NumInRange - 1)];
        }
        else
        {
            Spot = *std::max_element(Pool.begin(), Pool.end(), [&](ABuildingActor* A, ABuildingActor* B) { return NearestChosen(A) < NearestChosen(B); });
        }

        const FTransform SpawnTransform = Spot->GetTransform();
        AActor* Spawned = UGameplayStatics::BeginDeferredActorSpawnFromClass(Spot, BuildingToSpawn, SpawnTransform, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
        Spawned = Spawned ? UGameplayStatics::FinishSpawningActor(Spawned, SpawnTransform) : nullptr;
        if (Spawned)
        {
            Result->Add(Spawned);
            Used.push_back(Spot);
            Chosen.push_back(SpawnTransform.GetLocation());
            SpawnedClass = BuildingToSpawn;        }
    }

    if (bOutSucceeded)
    {
        *bOutSucceeded = Result->Num() == Entries->Num();
    }
    UE_LOG(LogFortWorld, Log, TEXT("Exploration actors: spawned %d of %d %s (%d placement actors to choose from)"), Result->Num(), Entries->Num(), SpawnedClass ? *SpawnedClass->GetName() : TEXT("None"), NumCandidates);
#endif
    return Result;
}
