#pragma once

#include "GameFramework/GameMode.h"

class AActor;
class AController;
class AFortGameSession;
class AFortWorldManager;
class APawn;

class AFortGameMode : public AGameMode
{
    DefineUnrealClass(AFortGameMode, FortniteGame)

public:
    DefineUProperty(AFortGameSession*, FortGameSession)
    DefineBitfieldUProperty(bWorldIsReady)
    DefineUProperty(UClass*, MissionManagerClass)

    TSubclassOf<AGameSession> GetGameSessionClass() const;
    APawn* SpawnDefaultPawnFor_Implementation(AController* NewPlayer, AActor* StartSpot);
    void FinishWorldInitialization(AFortWorldManager* WorldManager);
    void SetBadgeScores();

    static void Hook();

private:
    static TSubclassOf<AGameSession>* GetGameSessionClassHK(const AFortGameMode* This, TSubclassOf<AGameSession>* Result);
};
