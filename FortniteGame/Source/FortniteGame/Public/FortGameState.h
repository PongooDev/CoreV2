#pragma once

#include "GameFramework/GameState.h"

class AFortMissionManager;
class AFortWorldManager;
struct FFortMissionManagerRecord;

class AFortGameState : public AGameState
{
    DefineUnrealClass(AFortGameState, FortniteGame)

public:
    DefineUProperty(AFortMissionManager*, MissionManager)
    DefineUProperty(AFortWorldManager*, WorldManager)

    void SpawnMissionManager(const FFortMissionManagerRecord& MissionManagerRecord);

    static ptrdiff_t MissionManagerOffset();
};

class AFortGameStateZone : public AFortGameState
{
    DefineUnrealClass(AFortGameStateZone, FortniteGame)

public:
    DefineUProperty(float, GameDifficulty)
};
