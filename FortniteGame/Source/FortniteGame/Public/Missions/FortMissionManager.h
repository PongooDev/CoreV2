#pragma once

#include "GameFramework/Actor.h"
#include "Math/Rotator.h"
#include "Misc/Guid.h"
#include "Missions/FortMissionInfo.h"
#include "StructArray.h"

class UFortMissionGenerator;

enum class EMissionGenerationCategory : uint8_t
{
    Primary,
    Secondary,
    Tertiary,
    Survivor,
    Max_None,
};

struct FFortMissionPlacementItemLookupData
{
    DefineUnrealStruct(FFortMissionPlacementItemLookupData, FortniteGame)
    DefineUProperty(FGameplayTagContainer, ItemIdentifyingTags)
    DefineUProperty(FGameplayTagContainer, TagsAddedToPlacementActors)
    DefineUProperty(UClass*, ActorToPlace)
    DefineUProperty(AActor*, ActorToUseForSpawnLocation)
    DefineUProperty(AActor*, SpawnedActor)
    DefineUProperty(FVector, SpawnLocation)
    DefineUProperty(FRotator, SpawnRotation)
};

struct FFortMissionEntry
{
    DefineUnrealStruct(FFortMissionEntry, FortniteGame)
    DefineUProperty(float, Weight)
    DefineUProperty(int32_t, WorldMinLevel)
    DefineUProperty(int32_t, WorldMaxLevel)
    DefineUProperty(UFortMissionGenerator*, MissionGenerator)
    DefineUProperty(UFortMissionInfo*, MissionInfo)
    DefineUProperty(EMissionGenerationCategory, GenerationCategory)
    DefineUStructProperty(FFortGeneratedDifficultyOptions, GeneratedDifficultyOptions)
    DefineUStructArrayProperty(FFortMissionPlacementItemLookupData, BlueprintLookupData)
};

struct FFortMissionRecord
{
    DefineUnrealStruct(FFortMissionRecord, FortniteGame)
    DefineUStructProperty(FFortMissionEntry, MissionEntry)
    DefineUProperty(UFortMissionGenerator*, MissionGenerator)
    DefineUProperty(FGuid, MissionGuid)
};

struct FFortMissionManagerRecord
{
    DefineUnrealStruct(FFortMissionManagerRecord, FortniteGame)
    DefineUProperty(UClass*, MissionManagerClass)
    DefineUStructArrayProperty(FFortMissionRecord, MissionRecords)

    static void Construct(FFortMissionManagerRecord* This, UClass* InMissionManagerClass);
};

class AFortMission;

class AFortMissionManager : public AActor
{
    DefineUnrealClass(AFortMissionManager, FortniteGame)

public:
    DefineUProperty(TArray<AFortMission*>, Missions)

    void OnZoneEnd(bool bZoneCompleted);

    static void HookVTables();

private:
    static void OnZoneEndHK(AFortMissionManager* This, bool bZoneCompleted);
};
