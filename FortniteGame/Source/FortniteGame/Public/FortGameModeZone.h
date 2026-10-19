#pragma once

#include "FortGameMode.h"

class AFortAIGoalManager;

class AFortGameModeZone : public AFortGameMode
{
    DefineUnrealClass(AFortGameModeZone, FortniteGame)

public:
    DefineUProperty(AFortAIGoalManager*, AIGoalManager)

    APawn* SpawnDefaultPawnFor_Implementation(AController* NewPlayer, AActor* StartSpot);
    void FinishWorldInitialization(AFortWorldManager* WorldManager);

    static void Hook();
    static void HookVTables();

private:
    static APawn* SpawnDefaultPawnFor_ImplementationHK(AFortGameModeZone* This, AController* NewPlayer, AActor* StartSpot);
    static void FinishWorldInitializationHK(AFortGameModeZone* This, AFortWorldManager* WorldManager);
};
