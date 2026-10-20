#include "pch.h"
#include "FortGameModeZone.h"
#include "AI/FortAIDirector.h"
#include "AI/FortAIGoalManager.h"
#include "Engine/World.h"
#include "BlueprintGameplayTagLibrary.h"
#include "EnvironmentQuery/EnvQueryInstanceBlueprintWrapper.h"
#include "EnvironmentQuery/EnvQueryManager.h"
#include "FortAssetManager.h"
#include "FortGameData.h"
#include "FortWorldManager.h"
#include "GameThread.h"
#include "Logging/LogMacros.h"
#include "Building/BuildingActor.h"
#include "Templates/Casts.h"
#include "UObject/WeakObjectPtr.h"
#include "Finder.h"
#include "Hooks.h"
#include "Natives.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"

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

namespace
{
    constexpr int32_t SpawnPadQueryTicks = 600;

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

void AFortGameModeZone::PlacePlayerSpawnPad(AFortWorldManager* WorldManager)
{
#if WITH_SERVER_CODE
    UFortGameData* GameData = UFortAssetManager::Get().GetGameData();
    UEnvQuery* PlacementQuery = GameData ? GameData->DefaultPlayerSpawnPadPlacementData.PlacementQuery : nullptr;
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
    UFortGameData* GameData = UFortAssetManager::Get().GetGameData();
    if (!PlacementActor || !WorldManager || !GameData)
    {
        UE_LOG(LogFortPlacement, Error, TEXT("PlacePlayerSpawnPad: No valid placement actor could be found!  Failed to spawn player spawn pad."));
        return;
    }

    UE_LOG(LogFortPlacement, Log, TEXT("PlacePlayerSpawnPad: Found spot %s with initial tags"), *PlacementActor->GetName());

    FFortPlayerSpawnPadPlacementData& PlacementData = GameData->DefaultPlayerSpawnPadPlacementData;
    if (AActor* SpawnPad = WorldManager->SpawnPlayerSpawnPad(PlacementData, PlacementActor->GetTransform()))
    {
        FGameplayTagContainer& PlacementTags = PlacementActor->StaticGameplayTags;
        UBlueprintGameplayTagLibrary::AppendGameplayTagContainers(PlacementTags, PlacementData.TagsToAddToChosenPlacementActor);
        ActiveSpawnPad = SpawnPad;
    }
#endif
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
}

void AFortGameModeZone::CreateAIDirectorHK(AFortGameModeZone* This)
{
    This->CreateAIDirector();
}

APawn* AFortGameModeZone::SpawnDefaultPawnFor_ImplementationHK(AFortGameModeZone* This, AController* NewPlayer, AActor* StartSpot)
{
    return This->SpawnDefaultPawnFor_Implementation(NewPlayer, StartSpot);
}

void AFortGameModeZone::FinishWorldInitializationHK(AFortGameModeZone* This, AFortWorldManager* WorldManager)
{
    This->FinishWorldInitialization(WorldManager);
}
