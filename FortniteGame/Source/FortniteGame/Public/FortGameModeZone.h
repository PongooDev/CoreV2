#pragma once

#include "FortGameMode.h"

class AFortAIDirector;
class AFortAIGoalManager;
class UEnvQueryInstanceBlueprintWrapper;

class AFortGameModeZone : public AFortGameMode
{
    DefineUnrealClass(AFortGameModeZone, FortniteGame)

public:
    DefineUProperty(AFortAIDirector*, AIDirector)
    DefineUProperty(AFortAIGoalManager*, AIGoalManager)
    DefineUProperty(AActor*, ActiveSpawnPad)
    DefineUProperty(int32_t, OverrideAIDirectorIndex)

    APawn* SpawnDefaultPawnFor_Implementation(AController* NewPlayer, AActor* StartSpot);
    UClass* DetermineAIDirectorClass() const;
    void CreateAIDirector();
    void FinishWorldInitialization(AFortWorldManager* WorldManager);
    void PlacePlayerSpawnPad(AFortWorldManager* WorldManager);
    void OnPlayerSpawnPadQueryFinished(AFortWorldManager* WorldManager, UEnvQueryInstanceBlueprintWrapper* PlacementQuery);

    static void Hook();
    static void HookVTables();

private:
    static APawn* SpawnDefaultPawnFor_ImplementationHK(AFortGameModeZone* This, AController* NewPlayer, AActor* StartSpot);
    static void FinishWorldInitializationHK(AFortGameModeZone* This, AFortWorldManager* WorldManager);
    static void CreateAIDirectorHK(AFortGameModeZone* This);
};
