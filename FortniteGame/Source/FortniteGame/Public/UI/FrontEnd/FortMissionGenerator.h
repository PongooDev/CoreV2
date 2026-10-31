#pragma once

#include "FortGameData.h"
#include "StructArray.h"
#include "UObject/Object.h"
#include "UObject/SoftObjectPtr.h"

class UFortMissionInfo;

struct FFortPossibleMission
{
    DefineUnrealStruct(FFortPossibleMission, FortniteGame)
    DefineUProperty(TSoftObjectPtr<UFortMissionInfo>, MissionInfo)
    DefineUProperty(float, Weight)
    DefineUProperty(int32_t, MinAlwaysGenerated)
    DefineBitfieldUProperty(bIsPrototype)
};

class UFortMissionGenerator : public UObject
{
    DefineUnrealClass(UFortMissionGenerator, FortniteGame)

public:
    DefineUProperty(TSoftObjectPtr<UFortMissionInfo>, PrimaryMissionInfo)
    DefineUProperty(float, MissionGenerationChance)
    DefineUStructArrayProperty(FFortPossibleMission, SecondaryMissionList)
    DefineUStructArrayProperty(FFortPossibleMission, TertiaryMissionList)
    DefineUStructArrayProperty(FFortPossibleMission, SurvivorMissionList)
    DefineBitfieldUProperty(bUseOverridePlayerSpawnPadPlacementData)
    DefineUStructProperty(FFortPlayerSpawnPadPlacementData, OverridePlayerSpawnPadPlacementData)
};
