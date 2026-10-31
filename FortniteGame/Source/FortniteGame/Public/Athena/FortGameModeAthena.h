#pragma once

#include "FortGameModeZone.h"

class AController;
class AFortSafeZoneIndicator;
class AActor;
class ABuildingSMActor;
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
    DefineUProperty(bool, bSquadPlay)
    // Whether a dead player goes on to spectate (SpectateOnDeath).
    DefineUProperty(bool, bAllowSpectateAfterDeath)
    // Who PreLogin lets in ("Match is full" at it, 1.9 0x4319F0): the assigned players (OnGivenMatchAssignment,
    // 0x42D2B0).
    DefineUProperty(int32_t, MaxPlayerCount)
    // The warmup's length, and the shorter one it changes to once enough of MaxPlayerCount is in (UpdateWarmupPhase,
    // 1.9 0x43D9A0). Config values; no match option sets them.
    DefineUProperty(float, WarmupCountdownDuration)
    DefineUProperty(float, WarmupEarlyCountdownDuration)

    static bool HasMaxPlayerCount() { return Member_MaxPlayerCount.IsAvailable(); }
    static bool HasWarmupLength() { return Member_WarmupCountdownDuration.IsAvailable() && Member_WarmupEarlyCountdownDuration.IsAvailable(); }

    // Where a Battle Royale server starts: the Battle Royale zone theme's map and playlist
    // ("/Game/Athena/Maps/Athena_Terrain?playlistId=2" on 1.7.2), or empty when the build has none.
    static FString DefaultMatchURL();

    // The world's game mode when it is Battle Royale's, else null (Save the World and the builds before Battle Royale).
    static AFortGameModeAthena* GetCurrent(const UObject* WorldContextObject);

    // The loot tier group Battle Royale rolls in place of a container's or building's own (None: no loot).
    FName RedirectLootTierGroup(FName TierGroup);

    // Adds the match's playlist tag (Athena.Playlist.Solo, Duo, Squad or FiftyFifty, with its parents) to Tags unless
    // it is there, and says whether it added it.
    bool AddPlaylistContextTag(FGameplayTagContainer& Tags);

    APawn* SpawnDefaultPawnFor_Implementation(AController* NewPlayer, AActor* StartSpot);

    // Reports each player's place, kills and time alive to Core-Backend once their team is out of the match or has won,
    // for Battle Royale's stats (Core-Backend #59), as long as the game mode is there.
    void WatchMatchResults();

    // Keeps the warmup at the admin panel's length and, with late joining on, takes more players during it
    // (Core-Backend #122), as long as the game mode is there.
    void WatchWarmup();

    static void HookVTables();
    static void HookCallSites();
    static void HookBuildingVTables();

private:
    static APawn* SpawnDefaultPawnFor_ImplementationHK(AFortGameModeAthena* This, AController* NewPlayer, AActor* StartSpot);
    static void AircraftPhasePlayerCleanupCall(AController* Controller);
    static inline void (*ScoreBuildingConstructionOG)(AFortGameModeAthena* This, AController* Builder, ABuildingSMActor* Building) = nullptr;
    static void ScoreBuildingConstructionHK(AFortGameModeAthena* This, AController* Builder, ABuildingSMActor* Building);
};
