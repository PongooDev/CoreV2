#pragma once

#include "Containers/Array.h"
#include "GameFramework/Info.h"
#include "Math/Transform.h"
#include "Missions/FortMissionManager.h"

class UFortMissionInfo;
class UFortWorldRecord;
struct FFortPlayerSpawnPadPlacementData;

struct FFortStartingMissionInfo
{
    DefineUnrealStruct(FFortStartingMissionInfo, FortniteGame)
    DefineUProperty(TArray<UFortMissionInfo*>, StartingMissions)
};

enum class EFortWorldManagerState : uint8_t
{
    WMS_Created,
    WMS_QueryingWorld,
    WMS_WorldQueryComplete,
    WMS_CreatingNewWorld,
    WMS_LoadingExistingWorld,
    WMS_Running,
    WMS_Failed,
    WMS_MAX,
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

    void OnWorldRecordQueryComplete();
    void ServerTickLoadingOrCreatingWorld();
    void AddMissionsToMissionManagerRecord(const TArray<UFortMissionInfo*>& Missions);
    void CreateMissionManager();
    AActor* SpawnPlayerSpawnPad(const FFortPlayerSpawnPadPlacementData& PlacementData, const FTransform& SpawnTransform);

    static void Hook();

private:
    static inline void (*OnWorldRecordQueryCompleteOG)(AFortWorldManager* This) = nullptr;
    static void OnWorldRecordQueryCompleteHK(AFortWorldManager* This);
    static inline void (*ServerTickLoadingOrCreatingWorldOG)(AFortWorldManager* This) = nullptr;
    static void ServerTickLoadingOrCreatingWorldHK(AFortWorldManager* This);
};
