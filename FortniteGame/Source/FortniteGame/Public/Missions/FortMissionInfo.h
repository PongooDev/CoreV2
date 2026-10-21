#pragma once

#include "GameplayTagContainer.h"
#include "UObject/Object.h"

class AGameModeBase;
class UObject;

struct FFortGeneratedDifficultyOptions
{
    DefineUnrealStruct(FFortGeneratedDifficultyOptions, FortniteGame)
};

class UFortMissionInfo : public UObject
{
    DefineUnrealClass(UFortMissionInfo, FortniteGame)

public:
    DefineUProperty(uint8_t, MissionType)
    DefineUProperty(FGameplayTagContainer, MissionTags)
    DefineUProperty(int32_t, MaxMissionPoints)

    void GenerateMissionDifficultyOptions(FFortGeneratedDifficultyOptions& OutOptions, void* RandomStream, float GameDifficulty, AGameModeBase* GameMode, UObject* GenerationData);
};
