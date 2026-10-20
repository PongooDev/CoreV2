#pragma once

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
    void GenerateMissionDifficultyOptions(FFortGeneratedDifficultyOptions& OutOptions, void* RandomStream, float GameDifficulty, AGameModeBase* GameMode, UObject* GenerationData);
};
