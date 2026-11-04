#pragma once

#include "FortGameModeZone.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

class AController;
class AFortSafeZoneIndicator;
class AActor;
class ABuildingSMActor;
class AFortPlayerControllerAthena;
class AFortPawn;
class APawn;
class UObject;
struct FFortPlayerDeathReport;
class UDataTable;

// A playlist's data tables (from 1.11; Athena_GameMode_C's PlaylistDataTables): the tables whose rows
// ApplyPlaylistDataTables copies over the default ones (AthenaLootTierData_Client, AthenaLootPackages_Client and the rest)
// while the match's playlist is PlaylistId. From 3.0 the entry is only FortAthenaPlaylistClass, an AFortAthenaPlaylistBase
// class whose default object has the PlaylistId and the tables.
struct FAthenaPlaylistDataTableSet
{
    DefineUnrealStruct(FAthenaPlaylistDataTableSet, FortniteGame)
    DefineUProperty(int32_t, PlaylistId)
    DefineUProperty(UDataTable*, LootTierData)
    DefineUProperty(UDataTable*, LootPackages)
    DefineUProperty(UClass*, FortAthenaPlaylistClass)

    static bool HasPlaylistId() { return Member_PlaylistId.IsAvailable(); }
    static bool HasFortAthenaPlaylistClass() { return Member_FortAthenaPlaylistClass.IsAvailable(); }
};

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
    DefineUStructArrayProperty(FAthenaPlaylistDataTableSet, PlaylistDataTables)
    // The players still in the match (AddToAlivePlayers adds them, RemoveFromAlivePlayers takes them off).
    DefineUProperty(TArray<AFortPlayerControllerAthena*>, AlivePlayers)

    static bool HasMaxPlayerCount() { return Member_MaxPlayerCount.IsAvailable(); }

    // The most players a match here can have (the playlist's teams x their size; 0 when unknown), and the players
    // connected now (from their login on). Battle Royale's player bots fill the difference (FCoreAthenaBotManager).
    int32_t GetMatchCapacity();
    int32_t GetNumConnectedPlayers();
    // The playlist's team size and number of teams (the reservation beacon's NumPlayersPerTeam and NumTeams; 0 without one).
    int32_t GetPlayersPerTeam();
    int32_t GetNumTeams();
    // The players the match has places for, by team: each reservation's players (the reservation beacon's, the assigned
    // parties' and the late ones Core reserved) with their EFortTeam (HumanPvP_Team1 for beacon team 0) and account id,
    // those still loading in included. Empty without a beacon.
    std::vector<std::pair<uint8_t, std::wstring>> GetTeamReservations();
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

    // Battle Royale's changes to a dead player's death report, after AFortPlayerControllerZone::PrepareDeathReport
    // filled it (AFortPlayerControllerAthena's override calls it; 1.9.1 and 1.10 strip it).
    void ModifyDeathReport(AFortPlayerControllerAthena* Player, FFortPlayerDeathReport& DeathReport);

    // Every player still in the match outlived the one who is out (a player, or a player bot, CoreV2 #233): an
    // EFortQuestObjectiveStatEvent::AthenaOutlive event for each, as 1.8's game mode sends one per death (0x599DA0).
    void SendOutliveStatEvents(AFortPawn* DeadPawn);

    // Removes the placed chests and ammo boxes this match doesn't keep, from the map info's spawn percents (#205).
    void ApplyContainerSpawnPercents();

    // Reports each player's place, kills and time alive to Core-Backend once their team is out of the match or has won,
    // for Battle Royale's stats (Core-Backend #59), as long as the game mode is there.
    void WatchMatchResults();

    // Keeps the warmup at the admin panel's length and, with late joining on, takes more players during it
    // (Core-Backend #122), as long as the game mode is there.
    void WatchWarmup();

    // The game's own ApplyPlaylistDataTables (from 1.11), once per game mode, before the world's first loot roll: it
    // restores the default loot tables and copies the playlist's over them (the playlist the map URL gave, until the
    // match is assigned). Builds without it keep their tables.
    void ApplyPlaylistDataTablesOnce();

    // Once the match's assignment gives it another playlist, applies that playlist's tables and, when its loot tables
    // are other ones, rolls the world's loot again, as long as the match hasn't left its warmup.
    void WatchPlaylistDataTables();

    static void HookVTables();
    static void HookCallSites();
    static void HookBuildingVTables();

private:
    static APawn* SpawnDefaultPawnFor_ImplementationHK(AFortGameModeAthena* This, AController* NewPlayer, AActor* StartSpot);
    // The arguments are AFortPlayerController::ActivateSlot's; the 1.7.2 call it also takes passes only the controller.
    static void AircraftPhasePlayerCleanupCall(AController* Controller, uint8_t InQuickBar, int32_t Slot, float ActivateDelay, bool bUpdatePreviousFocusedSlot);
    static inline void (*ScoreBuildingConstructionOG)(AFortGameModeAthena* This, AController* Builder, ABuildingSMActor* Building) = nullptr;
    static void ScoreBuildingConstructionHK(AFortGameModeAthena* This, AController* Builder, ABuildingSMActor* Building);
};
