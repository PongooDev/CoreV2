#pragma once

#include "GameplayTagContainer.h"
#include "UObject/Object.h"

class AGameModeBase;
class UFortBadgeItemDefinition;
class UObject;

class EFortMissionType
{
    DefineUnrealEnum(EFortMissionType, FortniteGame)
    DefineUEnumValue(Primary)
    DefineUEnumValue(Secondary)
    DefineUEnumValue(Max_None)
};

struct FFortGeneratedDifficultyOptions
{
    DefineUnrealStruct(FFortGeneratedDifficultyOptions, FortniteGame)
};

class UFortMissionInfo : public UObject
{
    DefineUnrealClass(UFortMissionInfo, FortniteGame)

public:
    DefineUProperty(EFortMissionType, MissionType)
    DefineUProperty(FGameplayTagContainer, MissionTags)
    DefineUProperty(int32_t, MaxMissionPoints)
    DefineUProperty(UFortBadgeItemDefinition*, MissionRewardBadge)
    DefineUProperty(int32_t, ParTime)
    DefineUProperty(UFortBadgeItemDefinition*, UnderParBadge)

    void GenerateMissionDifficultyOptions(FFortGeneratedDifficultyOptions& OutOptions, void* RandomStream, float GameDifficulty, AGameModeBase* GameMode, UObject* GenerationData);
};
