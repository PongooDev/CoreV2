#pragma once

#include <chrono>

#include "Athena/AI/FortAthenaAIBotController.h"
#include "Math/Vector.h"
#include "UObject/WeakObjectPtr.h"

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

class ABuildingContainer;
class AFortGameModeAthena;
class AFortPickup;
class AFortPlayerControllerAthena;
class AFortPlayerStateAthena;
class APawn;
class UObject;

// The admin panel's player bot settings (Core-Backend's bots block, sent with the warmup answer: {count, level, health,
// shield, quests}, or null when "Force player AIs" is off).
struct FFortAthenaBotSettings
{
    int32_t Count = 0;
    int32_t Level = 5;
    float StartingHealth = 100.0f;
    float StartingShield = 0.0f;
    bool bEliminationsCountForQuests = true;
};

// A weapon, ammo or healing item pickup lying on the floor, as the bots' loot search sees it (refreshed every few
// seconds).
struct FFortAthenaBotLoot
{
    TWeakObjectPtr<AFortPickup> Pickup;
    FVector Location;
    bool bWeapon = false;
    bool bConsumable = false;
};

// A chest or ammo box the bots can search (21.40's _Loot looks for containers too; 8.30's bots didn't loot).
struct FFortAthenaBotContainer
{
    TWeakObjectPtr<ABuildingContainer> Container;
    FVector Location;
    bool bChest = false;
};

// A place bots land at (21.40's _SelectNextPOI): a named location (an AFortPoiVolume tagged Athena.Location.POI.*) or a
// cluster of chests, with its loot (chests, and a share of the floor weapons) and the bots sent there.
struct FFortAthenaBotLandingSpot
{
    FVector Location;
    float Radius = 6000.0f;
    std::wstring Name;
    float Loot = 0.0f;
    int32_t Bots = 0;
};

// Core's backport of Epic's UFortServerBotManagerAthena (8.30: the game mode's ServerBotManager, which registers each
// bot's controller, puts it on a team and handles its death), with 21.40's AFortGameModeAthena bot functions
// (PlaceBotOnTeam, AddToAliveBots, RemoveFromAliveBots, CheckWinConditionLastManStanding) for the bots' share of the
// match's counts. Not a UObject (3.0 has no such class): one per Battle Royale game mode, run from Core's game-thread tick.
//
// Bots only exist on builds without Epic's own (every build before 8.30, which has AFortAthenaAIBotController): a build
// with native player bots never uses this backport (HasNativePlayerBots). They're off unless the admin panel's "Force
// player AIs" is on.
class UFortServerBotManagerAthena
{
public:
    // Starts the manager for a Battle Royale game mode (FinishWorldInitialization): it waits for the backend's settings.
    static void Create(AFortGameModeAthena* GameMode);
    static UFortServerBotManagerAthena* Get();

    // Whether the build has Epic's own player bots (AFortAthenaAIBotController, 8.x on).
    static bool HasNativePlayerBots();

    // The backend's settings, from each warmup answer (null: off).
    void SetSettings(bool bEnabled, const FFortAthenaBotSettings& InSettings);
    const FFortAthenaBotSettings& GetSettings() const { return Settings; }
    bool IsEnabled() const { return bEnabled; }
    // Each frame, on the game thread (WatchManager).
    void Tick();

    // The bots still in the match (riding the aircraft, falling or on the ground).
    int32_t GetAliveBotCount() const;
    bool HasAliveBots() const { return GetAliveBotCount() > 0; }
    // The playlist's team size (1 solo, 2 duos, 4 squads), and a bot's teammates still in the match.
    int32_t GetTeamSize() const { return std::max(PlaylistTeamSize, 1); }
    std::vector<AFortAthenaAIBotController*> TeammatesOf(const AFortAthenaAIBotController* Bot) const;
    // The bot whose pawn or player state this is, or null.
    AFortAthenaAIBotController* FindBot(const UObject* PawnOrPlayerState) const;
    static bool IsBotPawn(const UObject* Object);

    // The bots a player eliminated (AFortGameModeAthena's match results add them to the player's kills).
    static int32_t BotKillsOf(const AFortPlayerStateAthena* PlayerState);
    // Battle Royale's quests: whether a Kill event against Target counts (the "bot eliminations count for quests" setting),
    // and the note that the game sent one (UFortQuestManager's stat events).
    static bool ShouldCountQuestEvent(const UObject* Target);
    static void NoteNativeKillEvent(const UObject* Target);

    // Core's death handler took a player out (AFortPlayerControllerAthena::OnPawnDied): whether the game's own win check
    // must be kept from running (bots still in the match), and the bots' recount and win check after it.
    bool ShouldHoldNativeWinCheck() const;
    void OnPlayerRemoved(AFortPlayerStateAthena* KillerState);

    // Server: the enemies a bot can fight, and the loot it can take (shared by every bot each tick).
    const std::vector<TWeakObjectPtr<AFortPawn>>& GetEnemyPawns() const { return EnemyPawns; }
    const std::vector<FFortAthenaBotLoot>& GetLoot() const { return Loot; }
    void ForgetLoot(AFortPickup* Pickup);
    // The island's unsearched chests and ammo boxes, and a container a bot searched (its loot is looked for next tick).
    const std::vector<FFortAthenaBotContainer>& GetContainers() const { return Containers; }
    void OnContainerSearched(ABuildingContainer* Container);
    // Navigation for the bots (Core's UAthenaNavSystem): whether the invoker navmesh is up.
    bool HasNavigation() const { return bHasNavigation; }
    AFortGameModeAthena* GetGameMode() const;
    // The next safe zone (its centre and radius), when there is one.
    bool GetSafeZone(FVector& OutCenter, float& OutRadius) const;
    // The storm circle as it is now, when there is one.
    bool GetCurrentSafeZone(FVector& OutCenter, float& OutRadius) const;
    // The storm's damage a second to a pawn outside the circle, for the safe zone phase.
    float GetStormDamagePerSecond() const;
    // A random spot on the ground near Around (a trace down from the sky), or false.
    bool FindGroundNear(const FVector& Around, float Radius, FVector& OutLocation) const;
    uint8_t TeamOfPawn(const APawn* Pawn) const;
    // The battle bus while it flies (found when the aircraft phase starts).
    AActor* GetAircraft() const { return reinterpret_cast<AActor*>(CurrentAircraft.Get()); }

    // A bot's shot hit Victim (a bot's or a player's pawn): its shield, then its health; at 0 it dies, the bot's
    // elimination (AFortPawn::ForceKill with the bot's controller).
    void ApplyBotDamage(AFortAthenaAIBotController* Attacker, AFortPawn* Victim, float Damage, UFortWeaponItemDefinition* Weapon);
    // Whether a bot's hit on Victim goes the game's way (its damage effect): a player's pawn always; a bot's in a solo
    // playlist (in team playlists Core's damage downs bots while a teammate stands).
    bool UsesGameDamage(const AFortPawn* Victim) const;
    // A bot hit Victim (LastBotAttackerOf).
    void NoteBotHit(AFortAthenaAIBotController* Attacker, AFortPawn* Victim);
    // The player state of the bot that last hit Pawn, in the last few seconds, or null.
    AFortPlayerStateAthena* LastBotAttackerOf(const AFortPawn* Pawn) const;

    // A bot's pawn died (AFortAthenaAIBotController::Tick saw it): its place, the kill, the counts and the win check.
    void RemoveFromAliveBots(AFortAthenaAIBotController* Bot, AFortPlayerStateAthena* KillerState);

    static FAIBotSkills SkillsForLevel(int32_t Level);

private:
    explicit UFortServerBotManagerAthena(AFortGameModeAthena* InGameMode);

    void TickWarmup(float Now);
    void StartBotMatchIfAsked();
    int32_t WantedBotCount() const;
    void ReadPlaylistTeams();
    int32_t MatchCapacity() const;
    void SpawnBots(int32_t Wanted, float Now);
    void RemoveSurplusBots(int32_t Wanted);
    bool RegisterBot(float Now);
    uint8_t PlaceBotOnTeam();
    void AddToAliveBots(AFortAthenaAIBotController* Bot);
    void OnAircraftPhaseStarted(float Now);
    void UpdateCounts();
    void CheckWinConditionLastManStanding();
    void RefreshEnemies();
    void RefreshLoot(float Now);
    void GatherContainers();
    // The landing spots (at the bus), the bus's path (its location sampled while it flies), and each bot's spot and
    // jump: spread over the reachable spots by their loot, or all at the best one with -CoreBotHotDrop.
    void GatherLandingSpots();
    void UpdateFlightPath(float Now);
    void AssignLandingSpots(float Now);
    void InitializeNavigation();
    void LogSummary(float Now);
    std::set<uint8_t> HumanTeams() const;
    std::set<uint8_t> AliveTeams(bool bIncludeHumans) const;

    FWeakObjectPtr GameMode;
    FWeakObjectPtr CurrentAircraft;
    bool bEnabled = false;
    bool bHasSettings = false;
    FFortAthenaBotSettings Settings;
    std::vector<std::unique_ptr<AFortAthenaAIBotController>> PlayerBots;
    int32_t NextBotIndex = 0;
    std::vector<FVector> WarmupSpots;
    uint8_t LastPhase = 0xFF;
    bool bAircraftStarted = false;
    bool bMatchEnded = false;
    bool bWarnedNative = false;
    bool bWarnedUnsupported = false;
    bool bLoggedCap = false;
    int32_t LastLoggedWanted = -1;
    int32_t BotsAtStart = 0;
    int32_t PlaylistTeams = 0;
    int32_t PlaylistTeamSize = 0;
    int32_t HumansAtStart = 0;
    int32_t LastPlayersLeft = -1;
    int32_t LastTeamsLeft = -1;
    float NextLootRefresh = 0.0f;
    float NextSummaryLog = 0.0f;
    float LastTickTime = -1.0f;
    float NextRegisterTime = 0.0f;
    bool bBotMatchStarted = false;
    bool bBotMatchAircraftStarted = false;
    std::vector<TWeakObjectPtr<AFortPawn>> EnemyPawns;
    std::vector<FFortAthenaBotLoot> Loot;
    std::vector<FFortAthenaBotContainer> Containers;
    std::vector<FFortAthenaBotLandingSpot> LandingSpots;
    bool bLandingAssigned = false;
    bool bHasFlightSample = false;
    FVector FlightSample;
    float FlightSampleTime = 0.0f;
    FVector FlightDirection;
    float FlightSpeed = 0.0f;
    float DropStartTime = 0.0f;
    float DropEndTime = 0.0f;
    bool bHasNavigation = false;
    std::chrono::steady_clock::time_point LastFrameStart;
    double FrameMsSum = 0.0;
    double FrameMsMax = 0.0;
    double BotsMsSum = 0.0;
    int32_t FrameCount = 0;
    std::map<UFortWeaponItemDefinition*, bool> GunDefinitions;
    struct FBotHit
    {
        TWeakObjectPtr<AFortPawn> Victim;
        TWeakObjectPtr<AFortPlayerStateAthena> Attacker;
        float Time = 0.0f;
    };
    std::vector<FBotHit> BotHits;
};
