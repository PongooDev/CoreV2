#pragma once

#include "FortGameData.h"
#include "StructArray.h"
#include "UObject/AssetPtr.h"
#include "UObject/Object.h"

class UFortMissionInfo;

struct FFortPossibleMission
{
    DefineUnrealStruct(FFortPossibleMission, FortniteGame)
    DefineUProperty(TAssetPtr<UFortMissionInfo>, MissionInfo)
    DefineUProperty(float, Weight)
    DefineUProperty(int32_t, MinAlwaysGenerated)
    DefineBitfieldUProperty(bIsPrototype)
};

class UFortMissionGenerator : public UObject
{
    DefineUnrealClass(UFortMissionGenerator, FortniteGame)

public:
    DefineUProperty(TAssetPtr<UFortMissionInfo>, PrimaryMissionInfo)
    DefineUProperty(float, MissionGenerationChance)
    DefineUStructArrayProperty(FFortPossibleMission, SecondaryMissionList)
    DefineUStructArrayProperty(FFortPossibleMission, TertiaryMissionList)
    DefineUStructArrayProperty(FFortPossibleMission, SurvivorMissionList)
    DefineBitfieldUProperty(bUseOverridePlayerSpawnPadPlacementData)
    DefineUStructProperty(FFortPlayerSpawnPadPlacementData, OverridePlayerSpawnPadPlacementData)
};
