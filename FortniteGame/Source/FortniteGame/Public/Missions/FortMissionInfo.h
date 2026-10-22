#pragma once

#include "GameplayTagContainer.h"
#include "UObject/Object.h"

class AGameModeBase;
class UFortBadgeItemDefinition;
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
    DefineUProperty(UFortBadgeItemDefinition*, MissionRewardBadge)
    DefineUProperty(int32_t, ParTime)
    DefineUProperty(UFortBadgeItemDefinition*, UnderParBadge)

    void GenerateMissionDifficultyOptions(FFortGeneratedDifficultyOptions& OutOptions, void* RandomStream, float GameDifficulty, AGameModeBase* GameMode, UObject* GenerationData);
};
