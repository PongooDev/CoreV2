#pragma once

#include "GameplayTagContainer.h"
#include "StructArray.h"
#include "UObject/AssetPtr.h"
#include "UObject/Object.h"

class AActor;
class AGameModeBase;
class UEnvQuery;
class UObject;

struct FFortGeneratedDifficultyOptions
{
    DefineUnrealStruct(FFortGeneratedDifficultyOptions, FortniteGame)
};

struct FFortMissionPlacementActorItem
{
    DefineUnrealStruct(FFortMissionPlacementActorItem, FortniteGame)
    DefineUProperty(FGameplayTagContainer, ItemIdentifyingTags)
    DefineUProperty(FGameplayTagContainer, TagsToAddToChosenPlacementActor)
    DefineUProperty(UEnvQuery*, PlacementQuery)
    DefineUProperty(TAssetSubclassOf<AActor>, ActorToPlace)
    DefineUProperty(int32_t, NumLocationsToFind)
    DefineBitfieldUProperty(bSpawnActorAutomatically)
};

struct FFortMissionPlacementItems
{
    DefineUnrealStruct(FFortMissionPlacementItems, FortniteGame)
    DefineUProperty(FGameplayTagContainer, TagsToAddToChosenPlacementActors)
    DefineUStructArrayProperty(FFortMissionPlacementActorItem, ActorsAndLocations)
};

class UFortMissionInfo : public UObject
{
    DefineUnrealClass(UFortMissionInfo, FortniteGame)

public:
    DefineUProperty(uint8_t, MissionType)
    DefineUProperty(FGameplayTagContainer, MissionTags)
    DefineUStructProperty(FFortMissionPlacementItems, MissionPlacementInfo)

    void GenerateMissionDifficultyOptions(FFortGeneratedDifficultyOptions& OutOptions, void* RandomStream, float GameDifficulty, AGameModeBase* GameMode, UObject* GenerationData);
};
