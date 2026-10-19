#pragma once

#include "FortGameMode.h"

class AFortAIGoalManager;
class UEnvQueryInstanceBlueprintWrapper;

class AFortGameModeZone : public AFortGameMode
{
    DefineUnrealClass(AFortGameModeZone, FortniteGame)

public:
    DefineUProperty(AFortAIGoalManager*, AIGoalManager)
    DefineUProperty(AActor*, ActiveSpawnPad)

    APawn* SpawnDefaultPawnFor_Implementation(AController* NewPlayer, AActor* StartSpot);
    void FinishWorldInitialization(AFortWorldManager* WorldManager);
    void PlacePlayerSpawnPad(AFortWorldManager* WorldManager);
    void OnPlayerSpawnPadQueryFinished(AFortWorldManager* WorldManager, UEnvQueryInstanceBlueprintWrapper* PlacementQuery);

    static void Hook();
    static void HookVTables();

private:
    static APawn* SpawnDefaultPawnFor_ImplementationHK(AFortGameModeZone* This, AController* NewPlayer, AActor* StartSpot);
    static void FinishWorldInitializationHK(AFortGameModeZone* This, AFortWorldManager* WorldManager);
};
