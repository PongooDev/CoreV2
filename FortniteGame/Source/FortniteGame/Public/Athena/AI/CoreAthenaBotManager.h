#pragma once

#include <chrono>

#include "Athena/AI/CoreAthenaBotController.h"
#include "Math/Vector.h"
#include "UObject/WeakObjectPtr.h"

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

class ABuildingContainer;
class AController;
class AFortGameModeAthena;
class AFortPickup;
class AFortPlayerControllerAthena;
class AFortPlayerPawnAthena;
class AFortPlayerStateAthena;
class APawn;
class UObject;
struct FGameplayTagContainer;

// The admin panel's player bot settings (Core-Backend's bots block, sent with the warmup answer: {count, level, health,
// shield, quests, fights, teams, noFill}, or null when "Force player AIs" and "Fill players' teams with bots" are off).
struct FCoreAthenaBotSettings
{
    // "Force player AIs": the bots on teams of their own (0 with the toggle off).
    int32_t Count = 0;
    int32_t Level = 5;
    float StartingHealth = 100.0f;
    float StartingShield = 0.0f;
    bool bEliminationsCountForQuests = true;
    // "Bot vs bot fight chance (%)": how often a bot takes on another bot it sees (doubled, up to 100, late in a match).
    int32_t BotFightChance = 30;
    // "Fill players' teams with bots" (separate from Force player AIs): in duos and squads, the empty places on a team of
    // players whose party fills are bots'. NoFillAccounts: the match's players whose party doesn't (the party leader's
    // matchmaking ticket's player.option.fillTeam=false, Squad Fill on No Fill), whose teams get none.
    bool bFillPlayerTeams = false;
    std::vector<std::wstring> NoFillAccounts;
};

// A weapon, ammo or healing item pickup lying on the floor, as the bots' loot search sees it (refreshed every few
// seconds).
struct FCoreAthenaBotLoot
{
    TWeakObjectPtr<AFortPickup> Pickup;
    FVector Location;
    bool bWeapon = false;
    bool bConsumable = false;
};

// A chest or ammo box the bots can search (21.40's _Loot looks for containers too; 8.30's bots didn't loot).
struct FCoreAthenaBotContainer
{
    TWeakObjectPtr<ABuildingContainer> Container;
    FVector Location;
    bool bChest = false;
};

// A place bots land at (21.40's _SelectNextPOI): a named location (an AFortPoiVolume tagged Athena.Location.POI.*) or a
// cluster of chests, with its loot (chests, and a share of the floor weapons) and the bots sent there.
struct FCoreAthenaBotLandingSpot
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
// player AIs" (bots on teams of their own) or "Fill players' teams with bots" (bots in the empty places of players' teams
// in duos and squads, PlayerTeamSlots) is on. The second is for every build: on one with native player bots it is the
// native system's to do, which Core doesn't drive yet, so this backport leaves it alone there too.
class FCoreAthenaBotManager
{
public:
    // Starts the manager for a Battle Royale game mode (FinishWorldInitialization): it waits for the backend's settings.
    static void Create(AFortGameModeAthena* GameMode);
    static FCoreAthenaBotManager* Get();

    // Whether the build has Epic's own player bots (AFortAthenaAIBotController, 8.x on).
    static bool HasNativePlayerBots();

    // The backend's settings, from each warmup answer (null: off).
    void SetSettings(bool bEnabled, const FCoreAthenaBotSettings& InSettings);
    const FCoreAthenaBotSettings& GetSettings() const { return Settings; }
    bool IsEnabled() const { return bEnabled; }
    // Each frame, on the game thread (WatchManager).
    void Tick();

    // The bots still in the match (riding the aircraft, falling or on the ground).
    int32_t GetAliveBotCount() const;
    bool HasAliveBots() const { return GetAliveBotCount() > 0; }
    // The playlist's team size (1 solo, 2 duos, 4 squads), and a bot's teammates still in the match.
    int32_t GetTeamSize() const { return std::max(PlaylistTeamSize, 1); }
    std::vector<FCoreAthenaBotController*> TeammatesOf(const FCoreAthenaBotController* Bot) const;
    // A bot's teammates who are players (a bot on a player's team, "Fill players' teams with bots"): their pawns, of the
    // ones still in the match.
    std::vector<AFortPlayerPawnAthena*> PlayerTeammatesOf(const FCoreAthenaBotController* Bot) const;
    // Whether a bot in a player's team's empty places is still in the match on Team (the team isn't out while one is).
    bool HasPlayersTeamBotsLeft(uint8_t Team) const;
    // The bot whose pawn or player state this is, or null.
    FCoreAthenaBotController* FindBot(const UObject* PawnOrPlayerState) const;
    static bool IsBotPawn(const UObject* Object);

    // The bots a player eliminated (AFortGameModeAthena's match results add them to the player's kills).
    static int32_t BotKillsOf(const AFortPlayerStateAthena* PlayerState);
    // Battle Royale's quests: whether a Kill event against Target counts (the "bot eliminations count for quests" setting),
    // and the note that the game sent one (UFortQuestManager's stat events).
    static bool ShouldCountQuestEvent(const UObject* Target);
    static void NoteNativeKillEvent(const UObject* Target);
    // Whether the bus hasn't left yet (the warmup island, where eliminations don't count).
    static bool IsWarmup();
    // A kill event's tags for a bot's elimination, as the game's kill event has them for a player's (3.1's
    // AFortGameModeZone::Killed, 0xB5E890): the bot's own tags (its pawn's, and its hero's class,
    // Homebase.Class.IsCommando...) for the target, the killer's weapon's (Weapon.Ranged.shotgun...) for the source.
    static void AddKillEventTags(const UObject* Target, AController* Killer, FGameplayTagContainer& TargetTags, FGameplayTagContainer& SourceTags);

    // Core's death handler took a player out (AFortPlayerControllerAthena::OnPawnDied): whether the game's own win check
    // must be kept from running (bots still in the match), and the bots' recount and win check after it.
    bool ShouldHoldNativeWinCheck() const;
    void OnPlayerRemoved(AFortPlayerStateAthena* KillerState);

    // Server: the enemies a bot can fight, and the loot it can take (shared by every bot each tick).
    const std::vector<TWeakObjectPtr<AFortPawn>>& GetEnemyPawns() const { return EnemyPawns; }
    const std::vector<FCoreAthenaBotLoot>& GetLoot() const { return Loot; }
    void ForgetLoot(AFortPickup* Pickup);
    // The island's unsearched chests and ammo boxes, and a container a bot searched (its loot is looked for next tick).
    const std::vector<FCoreAthenaBotContainer>& GetContainers() const { return Containers; }
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
    // A random warmup spot (a FortPlayerStartWarmup, where players respawn on the warmup island), or false.
    bool GetWarmupSpot(FVector& OutLocation) const;
    uint8_t TeamOfPawn(const APawn* Pawn) const;
    // The POIs (the landing spots, from the world's AFortPoiVolumes and its chest clusters, gathered at the bus): 21.40's
    // UFortAthenaAIBotEvaluator_SelectNextPOI::FindNextBestPOI, the best spot near From the bot hasn't been to, with
    // loot left and inside the next safe zone (-1: none); the spot Location is in (-1: none); and a spot by its index.
    int32_t FindNextBestPOI(const FVector& From, const std::vector<int32_t>& Visited) const;
    int32_t FindPOIAt(const FVector& Location) const;
    const FCoreAthenaBotLandingSpot* GetPOI(int32_t Index) const;
    // How many bots are heading for Target (FCoreAthenaBotController::Investigate), other than Except.
    int32_t CountHuntersOf(const AFortPawn* Target, const FCoreAthenaBotController* Except) const;
    // The battle bus while it flies (found when the aircraft phase starts).
    AActor* GetAircraft() const { return reinterpret_cast<AActor*>(CurrentAircraft.Get()); }

    // A bot's shot hit Victim (a bot's or a player's pawn): its shield, then its health; at 0 it dies, the bot's
    // elimination (AFortPawn::ForceKill with the bot's controller).
    void ApplyBotDamage(FCoreAthenaBotController* Attacker, AFortPawn* Victim, float Damage, UFortWeaponItemDefinition* Weapon);
    // Whether a bot's hit on Victim goes the game's way (its damage effect): a player's pawn always; a bot's in a solo
    // playlist (in team playlists Core's damage downs bots while a teammate stands).
    bool UsesGameDamage(const AFortPawn* Victim) const;
    // A bot hit Victim (LastBotAttackerOf).
    void NoteBotHit(FCoreAthenaBotController* Attacker, AFortPawn* Victim);
    // The player state of the bot that last hit Pawn, in the last few seconds, or null.
    AFortPlayerStateAthena* LastBotAttackerOf(const AFortPawn* Pawn) const;

    // A bot's pawn died (FCoreAthenaBotController::Tick saw it): its place, the kill, the counts and the win check.
    void RemoveFromAliveBots(FCoreAthenaBotController* Bot, AFortPlayerStateAthena* KillerState);

    static FCoreAIBotSkills SkillsForLevel(int32_t Level);

private:
    explicit FCoreAthenaBotManager(AFortGameModeAthena* InGameMode);

    void TickWarmup(float Now);
    void StartBotMatchIfAsked();
    // The bots on teams of their own (Force player AIs), with PlayerTeamBots places on players' teams taken.
    int32_t WantedBotCount(int32_t PlayerTeamBots) const;
    // "Fill players' teams with bots": each player's team (duos and squads) whose players' parties all fill, and the
    // places on it no player has (the players on it and those its reservations still bring), for bots.
    std::map<uint8_t, int32_t> PlayerTeamSlots() const;
    // The bots alive on players' teams (bPlayersTeams) or on teams of their own.
    int32_t CountAliveBots(bool bPlayersTeams) const;
    // After the bus: the players' teams' bots jump with their players and land by them; and each takes its team's squad.
    void UpdatePlayerTeamBots(float Now);
    // A player's team is out: its players' place (the game's SetTeamPlace on one of them, else their player states).
    void SetPlayersTeamPlace(uint8_t Team, int32_t Place);
    void ReadPlaylistTeams();
    int32_t MatchCapacity() const;
    // One bot more, a player's team first (TeamSlots), else on a team of its own while fewer than Wanted are.
    bool SpawnNextBot(int32_t Wanted, const std::map<uint8_t, int32_t>& TeamSlots, float Now);
    // A player (not a bot) has a pawn on the island.
    bool HasHumanSpawned() const;
    void RemoveSurplusBots(int32_t Wanted, const std::map<uint8_t, int32_t>& TeamSlots);
    bool RegisterBot(float Now, uint8_t Team, bool bPlayersTeam);
    uint8_t PlaceBotOnTeam();
    void AddToAliveBots(FCoreAthenaBotController* Bot);
    void OnAircraftPhaseStarted(float Now);
    void UpdateCounts();
    void CheckWinConditionLastManStanding();
    void RefreshEnemies();
    void RefreshLoot(float Now);
    void GatherContainers();
    // The landing spots (at the bus), the bus's path (its location sampled while it flies), and each bot's spot and
    // jump: spread over the reachable spots by their loot, or all at the best one with -CoreBotHotDrop.
    void GatherLandingSpots();
    // The loot left around a spot: its unsearched chests and ammo boxes, and the floor weapons.
    float LootAround(const FVector& Center, float Radius) const;
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
    FCoreAthenaBotSettings Settings;
    std::vector<std::unique_ptr<FCoreAthenaBotController>> PlayerBots;
    int32_t NextBotIndex = 0;
    // This match's shuffled order of the name list.
    std::vector<int32_t> NameOrder;
    std::vector<FVector> WarmupSpots;
    uint8_t LastPhase = 0xFF;
    bool bAircraftStarted = false;
    bool bMatchEnded = false;
    bool bLoggedKillTags = false;
    bool bWarnedNative = false;
    bool bWarnedUnsupported = false;
    bool bLoggedCap = false;
    int32_t LastLoggedWanted = -1;
    int32_t LastLoggedTeamBots = -1;
    float NextPlayerTeamSync = 0.0f;
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
    // Paced spawning in the warmup: the next bot's time, the players connected last tick, and the first spawn's log.
    float NextPacedSpawn = 0.0f;
    int32_t LastHumanCount = 0;
    bool bLoggedSpawnStart = false;
    bool bBotMatchStarted = false;
    bool bBotMatchAircraftStarted = false;
    std::vector<TWeakObjectPtr<AFortPawn>> EnemyPawns;
    std::vector<FCoreAthenaBotLoot> Loot;
    std::vector<FCoreAthenaBotContainer> Containers;
    std::vector<FCoreAthenaBotLandingSpot> LandingSpots;
    bool bLandingAssigned = false;
    bool bHasFlightSample = false;
    FVector FlightSample;
    float FlightSampleTime = 0.0f;
    FVector FlightDirection;
    float FlightSpeed = 0.0f;
    float DropStartTime = 0.0f;
    float DropEndTime = 0.0f;
    bool bHasNavigation = false;
    bool bNavigationTried = false;
    std::chrono::steady_clock::time_point LastFrameStart;
    double FrameMsSum = 0.0;
    double FrameMsMax = 0.0;
    double BotsMsSum = 0.0;
    int32_t FrameCount = 0;
    // Hitches in the summary's window: frames over 50 ms, the bots' worst frame, and what took longest in it.
    int32_t FrameSpikes = 0;
    double BotsMsMax = 0.0;
    double LootRefreshMsMax = 0.0;
    double BotTickMsMax = 0.0;
    std::wstring SlowestBotTick;
    std::map<UFortWeaponItemDefinition*, bool> GunDefinitions;
    struct FBotHit
    {
        TWeakObjectPtr<AFortPawn> Victim;
        TWeakObjectPtr<AFortPlayerStateAthena> Attacker;
        float Time = 0.0f;
    };
    std::vector<FBotHit> BotHits;
};
