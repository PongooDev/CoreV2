#pragma once

#include "FortGameModeZone.h"

class AController;
class AActor;
class APawn;

// Battle Royale's game mode (from 1.7.2; Athena_GameMode_C on Athena_Terrain). Its flow (warmup, aircraft, safe zones,
// end game) is native; what the shipping exe stripped from it is restored here (Core-Docs Athena-Gameplay-1.7.2.md).
class AFortGameModeAthena : public AFortGameModeZone
{
    DefineUnrealClass(AFortGameModeAthena, FortniteGame)

public:
    static void HookVTables();

private:
    static APawn* SpawnDefaultPawnFor_ImplementationHK(AFortGameModeAthena* This, AController* NewPlayer, AActor* StartSpot);
};
