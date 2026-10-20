#pragma once

#include "GameFramework/Actor.h"
#include "Misc/Guid.h"
#include "StructArray.h"

class UFortMissionInfo;

enum class EMissionGenerationCategory : uint8_t
{
    Primary,
    Secondary,
    Tertiary,
    Survivor,
    Max_None,
};

struct FFortMissionEntry
{
    DefineUnrealStruct(FFortMissionEntry, FortniteGame)
    DefineUProperty(float, Weight)
    DefineUProperty(int32_t, WorldMinLevel)
    DefineUProperty(int32_t, WorldMaxLevel)
    DefineUProperty(UFortMissionInfo*, MissionInfo)
    DefineUProperty(EMissionGenerationCategory, GenerationCategory)
};

struct FFortMissionRecord
{
    DefineUnrealStruct(FFortMissionRecord, FortniteGame)
    DefineUStructProperty(FFortMissionEntry, MissionEntry)
    DefineUProperty(FGuid, MissionGuid)
};

struct FFortMissionManagerRecord
{
    DefineUnrealStruct(FFortMissionManagerRecord, FortniteGame)
    DefineUProperty(UClass*, MissionManagerClass)
    DefineUStructArrayProperty(FFortMissionRecord, MissionRecords)

    static void Construct(FFortMissionManagerRecord* This, UClass* InMissionManagerClass);
};

class AFortMissionManager : public AActor
{
    DefineUnrealClass(AFortMissionManager, FortniteGame)
};
