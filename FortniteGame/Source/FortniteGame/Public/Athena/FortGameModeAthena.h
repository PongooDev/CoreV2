#pragma once

#include "FortGameModeZone.h"

class AController;
class AFortSafeZoneIndicator;
class AActor;
class APawn;
class UObject;

// Battle Royale's game mode (from 1.7.2; Athena_GameMode_C on Athena_Terrain). Its flow (warmup, aircraft, safe zones,
// end game) is native; what the shipping exe stripped from it is restored here (Core-Docs Athena-Gameplay-1.7.2.md).
class AFortGameModeAthena : public AFortGameModeZone
{
    DefineUnrealClass(AFortGameModeAthena, FortniteGame)

public:
    DefineUProperty(int32_t, SafeZonePhase)
    DefineUProperty(AFortSafeZoneIndicator*, SafeZoneIndicator)
    // Each safe zone's center, picked when the storm starts (index: the phase).
    DefineUProperty(TArray<FVector>, SafeZoneLocations)

    // Where a Battle Royale server starts: the Battle Royale zone theme's map and playlist
    // ("/Game/Athena/Maps/Athena_Terrain?playlistId=2" on 1.7.2), or empty when the build has none.
    static FString DefaultMatchURL();

    // The world's game mode when it is Battle Royale's, else null (Save the World and the builds before Battle Royale).
    static AFortGameModeAthena* GetCurrent(const UObject* WorldContextObject);

    // The loot tier group Battle Royale rolls in place of a container's or building's own (None: no loot).
    FName RedirectLootTierGroup(FName TierGroup);

    APawn* SpawnDefaultPawnFor_Implementation(AController* NewPlayer, AActor* StartSpot);

    // Reports each player's place, kills and time alive to Core-Backend once their team is out of the match or has won,
    // for Battle Royale's stats (Core-Backend #59), as long as the game mode is there.
    void WatchMatchResults();

    static void HookVTables();
    static void HookCallSites();

private:
    static APawn* SpawnDefaultPawnFor_ImplementationHK(AFortGameModeAthena* This, AController* NewPlayer, AActor* StartSpot);
    static void AircraftPhasePlayerCleanupCall(AController* Controller);
};
