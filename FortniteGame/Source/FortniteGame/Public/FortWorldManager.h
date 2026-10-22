#pragma once

#include "Containers/Array.h"
#include "GameFramework/Info.h"
#include "Math/Transform.h"
#include "Missions/FortMissionManager.h"

class AFortPlayerController;
class UFortMissionGenerator;
class UFortMissionInfo;
class UFortWorldRecord;
struct FFortPlayerSpawnPadPlacementData;
struct FQuickBar;

struct FFortStartingMissionInfo
{
    DefineUnrealStruct(FFortStartingMissionInfo, FortniteGame)
    DefineUProperty(TArray<UFortMissionInfo*>, StartingMissions)
    DefineBitfieldUProperty(bDisableSharedMissionLoading)
};

class EFortWorldManagerState
{
    DefineUnrealEnum(EFortWorldManagerState, FortniteGame)
    DefineUEnumValue(WMS_Created)
    DefineUEnumValue(WMS_QueryingWorld)
    DefineUEnumValue(WMS_WorldQueryComplete)
    DefineUEnumValue(WMS_CreatingNewWorld)
    DefineUEnumValue(WMS_LoadingExistingWorld)
    DefineUEnumValue(WMS_Running)
    DefineUEnumValue(WMS_Failed)
    DefineUEnumValue(WMS_MAX)
};

class AFortWorldManager : public AInfo
{
    DefineUnrealClass(AFortWorldManager, FortniteGame)

public:
    DefineUProperty(EFortWorldManagerState, WorldManagerState)
    DefineUProperty(UFortWorldRecord*, CurrentWorldRecord)
    DefineUStructProperty(FFortStartingMissionInfo, StartingMissionInfo)
    DefineUStructProperty(FFortMissionManagerRecord, MissionManagerRecord)
    DefineBitfieldUProperty(bHasCreatedMissions)
    DefineBitfieldUProperty(bCreatedMissionRecord)
    DefineUProperty(bool, bZoneFinished)
    DefineUProperty(bool, bZoneCompleted)

    static ptrdiff_t ZoneFinishedOffset();
    static ptrdiff_t ZoneCompletedOffset();

    void OnWorldRecordQueryComplete();
    void ServerTickLoadingOrCreatingWorld();
    void AddSharedMissionsToMissionManagerRecord();
    void AddMissionsToMissionManagerRecord(const TArray<UFortMissionInfo*>& Missions, EMissionGenerationCategory GenerationCategory = EMissionGenerationCategory::GetMax_None(), UFortMissionGenerator* MissionGenerator = nullptr);
    void CreateMissionManager();
    int32_t GetOrAllocatePlayerID(const FString& UniqueId, const FString& DisplayName);
    AActor* SpawnPlayerSpawnPad(const FFortPlayerSpawnPadPlacementData& PlacementData, const FTransform& SpawnTransform);
    bool UpdatePlayerQuickBarRecords(AFortPlayerController* PlayerController);
    bool RestorePlayerQuickBars(AFortPlayerController* PlayerController);

    static void Hook();
    static void HookCallSites();

private:
    static inline void (*OnWorldRecordQueryCompleteOG)(AFortWorldManager* This) = nullptr;
    static void OnWorldRecordQueryCompleteHK(AFortWorldManager* This);
    static inline void (*ServerTickLoadingOrCreatingWorldOG)(AFortWorldManager* This) = nullptr;
    static void ServerTickLoadingOrCreatingWorldHK(AFortWorldManager* This);
    static bool UpdatePlayerQuickBarRecordsCall(AFortWorldManager* This, AFortPlayerController* PlayerController);

    bool GetPlayerQuickBarRecords(AFortPlayerController* PlayerController, FQuickBar*& OutPrimary, FQuickBar*& OutSecondary);
};
