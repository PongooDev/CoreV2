#pragma once

#include "Containers/Array.h"
#include "FortTheaterInfo.h"
#include "FortZoneTheme.h"
#include "GameplayTagContainer.h"
#include "StructArray.h"
#include "UObject/AssetPtr.h"
#include "UObject/Object.h"

class UFortMissionInfo;

struct FFortGlobalMission
{
    DefineUnrealStruct(FFortGlobalMission, FortniteGame)
    DefineUProperty(TAssetPtr<UFortMissionInfo>, MissionInfo)
    DefineUProperty(TArray<EFortTheaterType>, AllowedTheaterTypes)
    DefineUProperty(FGameplayTagQuery, TheaterTagQuery)
    DefineUProperty(FGameplayTagQuery, RegionTagQuery)
    DefineUProperty(TArray<EFortZoneType>, AllowedZoneTypes)
    DefineUProperty(FGameplayTagQuery, ZoneTagQuery)
    DefineUProperty(FGameplayTagQuery, PrimaryMissionTagQuery)
    DefineUProperty(float, MaxDifficultyLevel)
    DefineUProperty(float, MinDifficultyLevel)
    DefineBitfieldUProperty(bIsPrototype)
    DefineBitfieldUProperty(bAllowInTestMaps)
};

class UFortSharedMissionLists : public UObject
{
    DefineUnrealClass(UFortSharedMissionLists, FortniteGame)

public:
    DefineUStructArrayProperty(FFortGlobalMission, SharedMissionList)
};
