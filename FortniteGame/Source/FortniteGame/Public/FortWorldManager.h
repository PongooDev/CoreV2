#pragma once

#include "GameFramework/Info.h"
#include "Math/Transform.h"

class UFortWorldRecord;
struct FFortPlayerSpawnPadPlacementData;

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

    void OnWorldRecordQueryComplete();
    AActor* SpawnPlayerSpawnPad(const FFortPlayerSpawnPadPlacementData& PlacementData, const FTransform& SpawnTransform);

    static void Hook();

private:
    static inline void (*OnWorldRecordQueryCompleteOG)(AFortWorldManager* This) = nullptr;
    static void OnWorldRecordQueryCompleteHK(AFortWorldManager* This);
};
