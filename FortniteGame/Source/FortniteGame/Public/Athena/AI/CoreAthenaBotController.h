#pragma once

#include "Math/Vector.h"
#include "Misc/Guid.h"
#include "UObject/NameTypes.h"
#include "UObject/WeakObjectPtr.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

class AAIController;
class AActor;
class ABuildingContainer;
class ABuildingSMActor;
class AFortAthenaAircraft;
class AFortGameModeAthena;
class AFortPawn;
class AFortPickup;
class AFortPlayerPawnAthena;
class AFortPlayerStateAthena;
class AFortWeapon;
class UAbilitySystemComponent;
class UAthenaPickaxeItemDefinition;
class UFortItemDefinition;
class UObject;
class UFortWeaponItemDefinition;
class FCoreAthenaBotManager;

// Core's backport of Epic's Battle Royale player bots for builds before 8.30 (CoreV2 #233, Core-Docs
// Athena-Bots-Plan.md), named after Epic's design but under Core names: Epic's own names (AFortAthenaAIBotController,
// UFortServerBotManagerAthena, ...) are kept for the wrappers over the game's real classes from 8.30. Epic's
// AFortAthenaAIBotController (8.30, CL-5793395) is a UClass deriving from AAIController; builds before 8.30 have none of
// the Athena AI classes and Core can't add UClasses, so each bot is a stock AAIController (with a player state)
// possessing an ordinary AFortPlayerPawnAthena, and this C++ class is its "brain": 8.30's controller fields
// (CurrentTarget, CurrentAlertLevel, WeaponList, Inventory, AIBotSkills) and its behaviour tree's evaluators, as
// functions named after 8.30's and 21.40's UFortAthenaAIBotEvaluator_* classes, run from Core's game-thread tick.
// The builds this serves have none of 8.30's tuning, so the numbers here are Core's own 🟡. From 8.30 Core will use the
// native system instead (the game's own bot manager, controller and behaviour trees, which ship in the PC paks).

// How a bot builds (Chapter 2's player bots), by skill level: 21.40's UFortAthenaAIBotBuildingDigestedSkillSet (the
// defensive builds' timing and WeightedBuildingLists) and its UFortAthenaAIPerk_Bunker (FFortAIRuntimePerkInfo_Bunker: the
// 1x1 box while healing or under fire from an unseen enemy). Epic's are FScalableFloats over the skill level (Digest);
// their values ship only in later builds' bot data, so these are Core's 🟡, filled by FCoreAthenaBotManager::SkillsForLevel.
struct FCoreAthenaBotBuildingSkills
{
    // UFortAthenaAIBotBuildingDigestedSkillSet.
    float DefensiveBuildingDelayBetweenBuilds = 15.0f;
    float DefensiveBuildingDelayDeviationBetweenBuilds = 5.0f;
    float DelayBetweenBuildPieces = 0.35f;
    // Core's: the share of hits in the open that the bot answers with a build (Epic's evaluator only has its cooldown).
    float DefensiveBuildingOdds = 0.33f;
    // FAthenaFortAIBotWeightedBuildingList::Weight of the two lists Core's bots build from when shot: a wall towards the
    // shooter, and that wall with a ramp up to it behind (the bunker's forward ramp).
    float WallListWeight = 0.7f;
    float WallAndRampListWeight = 0.3f;
    // UFortAthenaAIPerk_Bunker (FFortAIRuntimePerkInfo's OddsToActivate and CooldownDuration for the box while healing).
    float BunkerOddsToActivate = 0.4f;
    float BunkerCooldownDuration = 45.0f;
    float OddsToBeUsedAgainstUnknownThreat = 0.2f;
    float OddsToUseRoofBuilding = 0.5f;
    float OddsToBuildForwardRamp = 0.35f;
    float InitialBuildDelay = 0.4f;
    float SamePieceBuildDelay = 0.3f;
    float SamePieceBuildDelayRandomDeviation = 0.1f;
    float DifferentPieceBuildDelay = 0.45f;
    float DifferentPieceBuildDelayRandomDeviation = 0.15f;
};

// 8.30's FAIBotSkills (0x14 bytes: aim timing and tracking), which a bot's controller blueprint sets; Core fills it from
// the admin panel's bot level (FCoreAthenaBotManager::SkillsForLevel), with the sight range and aim error 21.40
// keeps in its Perception and Aiming skill sets.
struct FCoreAIBotSkills
{
    float MaxTimeBetweenShots = 0.4f;
    float DeviationTimeBetweenShots = 0.2f;
    float MaxTrackingReactionTime = 0.6f;
    float MaxTrackingInterpTime = 0.3f;
    float MaxTrackingPredictionError = 150.0f;
    // Core's: how far the bot sees an enemy.
    float SightRange = 6000.0f;
    // 21.40's UFortAthenaAIBotAimingSkillSet / FWeaponAccuracy terms (its values aren't in builds before 8.30, so Core's 🟡):
    // the spread of a shot at the target (one standard deviation, in units across the line of fire), a base
    // (MaxTrackingOffsetError) plus a share per 10 m up to 25 m (MaxTrackingDistanceNearError) and per 10 m beyond it
    // (MaxTrackingDistanceFarError). The vertical spread is MaxTrackingHeightOffsetError times the horizontal.
    float MaxTrackingOffsetError = 48.0f;
    float MaxTrackingDistanceNearError = 23.0f;
    float MaxTrackingDistanceFarError = 33.0f;
    float MaxTrackingHeightOffsetError = 0.6f;
    FCoreAthenaBotBuildingSkills Building;
};

// 21.40's EOrientedConstructionBuildingType (its values aren't in the PDB; Epic's has walls along X and Y, floors and
// ramps), as Core's bots use it: a piece in a grid cell, oriented by the build's forward (towards the threat).
enum class ECoreOrientedConstructionBuildingType : uint8_t
{
    WallForward,
    WallRight,
    WallBack,
    WallLeft,
    // A ramp rising towards the forward edge (BR's ramp, PBWA_*1_StairW).
    RampForward,
    // A cone roof on the cell (BR's cone, PBWA_*1_RoofC), a level up.
    Roof,
};

// 21.40's FFortAthenaAIBotBuildDescriptor: a piece and its grid cell (Location), here relative to the build's origin cell
// (cells forward, cells to the right, levels up).
struct FCoreAthenaBotBuildDescriptor
{
    int32_t Location[3] = {};
    ECoreOrientedConstructionBuildingType BuildingType = ECoreOrientedConstructionBuildingType::WallForward;
};

// 21.40's FFortAthenaAIBotBuildingQueue (UFortAthenaAIBotBuildingComponent::BuildQueue): the pieces to build one after
// another, the delays between them, and Core's frame for them (the origin cell on the 512 uu building grid, the ground
// there, the forward axis, the material).
struct FCoreAthenaBotBuildingQueue
{
    std::vector<FCoreAthenaBotBuildDescriptor> Descriptors;
    float InitialBuildDelay = 0.0f;
    float SamePieceBuildDelay = 0.0f;
    float SamePieceBuildDelayRandomDeviation = 0.0f;
    float DifferentPieceBuildDelay = 0.0f;
    float DifferentPieceBuildDelayRandomDeviation = 0.0f;
    // The origin cell's lower corner on the game's structural grid (UBuildingStructuralSupportSystem's grid box): its
    // cells are 512 uu, centred on the grid origin plus whole cells, and its levels 384 uu up from the origin.
    float CellMinX = 0.0f;
    float CellMinY = 0.0f;
    float BaseZ = 0.0f;
    // 0 +X, 1 +Y, 2 -X, 3 -Y.
    int32_t Forward = 0;
    // 0 wood, 1 stone, 2 metal.
    int32_t MaterialKind = 0;
    const wchar_t* Reason = L"";
    // A way out for a stuck bot (TryToUnstuck's ramps up a hill or out of a basement): its pieces may sit in the
    // landscape, as a player's ramp up a slope does, but not in anyone's piece or a house, and the storm doesn't end it.
    bool bToUnstick = false;
};

// 8.30's EAlertLevel (the controller's CurrentAlertLevel, OnPerceptionSensed's state machine).
enum class ECoreAlertLevel : uint8_t
{
    Unaware,
    Suspicious,
    Alerted,
    Threatened,
};

// Where a bot is in the match (Core's: 8.30's bots only ever spawned on the ground).
enum class ECoreAthenaBotState : uint8_t
{
    Warmup,
    InAircraft,
    Skydiving,
    Landed,
    Dead,
};

// The kind of gun, which decides the range a bot uses it at (from the weapon definition's name: WID_Shotgun_*,
// WID_Assault_*, WID_Sniper_*, WID_Pistol_AutoHeavy* and WID_Pistol_Scavenger* are 3.x's SMGs, WID_Launcher_*).
enum class ECoreAthenaBotWeaponType : uint8_t
{
    Other,
    Pistol,
    SMG,
    AssaultRifle,
    Shotgun,
    Sniper,
    Launcher,
};

// What a bot on the ground is set on doing, out of a fight (Core's: 21.40's behaviour tree runs one evaluator's task at a
// time). A goal it chose is kept until it is done or no longer applies, so the bot doesn't turn back and forth between
// two of them; only something close by (a pickup within 15 m, healing, a teammate down) comes first meanwhile.
enum class ECoreAthenaBotGoal : uint8_t
{
    None,
    Loot,
    Search,
    Storm,
    Regroup,
    Harvest,
    // 21.40's UFortAthenaAIBotEvaluator_Investigate: an opponent the bot heads for.
    Hunt,
    // 21.40's UFortAthenaAIBotEvaluator_SelectNextPOI: the next named location or chest cluster, once this one is looted.
    TravelToPOI,
    Wander,
};

// One of the bot's weapons (8.30's FFortAthenaInventoryInformation is the weapon list's entry: EquipMaxDistance and
// WeaponDefinition; the item itself lives in the controller's own AFortInventory there, in Core's brain here).
struct FCoreAthenaBotWeapon
{
    UFortWeaponItemDefinition* WeaponDefinition = nullptr;
    UFortItemDefinition* AmmoDefinition = nullptr;
    FGuid ItemGuid;
    int32_t LoadedAmmo = 0;
    // The definition's EFortRarity (UFortItemDefinition::GetRarity: 0 common to 4 legendary in Battle Royale).
    uint8_t Rarity = 0;
    // From its stats row (FFortBaseWeaponStats::DmgPB, ClipSize, ReloadTime, ReloadType;
    // FFortRangedWeaponStats::BulletsPerCartridge and FiringRate).
    int32_t ClipSize = 0;
    float Damage = 20.0f;
    int32_t Pellets = 1;
    float FiringRate = 2.0f;
    float ReloadTime = 1.6f;
    // EFortWeaponReloadType::ReloadIndividualBullets: a pump shotgun or a revolver loads its shells one at a time, so a
    // partial reload takes its share of ReloadTime.
    bool bReloadIndividualBullets = false;
    ECoreAthenaBotWeaponType Type = ECoreAthenaBotWeaponType::Other;
    // 8.30's FFortAthenaInventoryInformation::EquipMaxDistance: the farthest target the bot takes this weapon out for.
    float EquipMaxDistance = 5000.0f;
    // When this gun can fire again (its FiringRate from its last shot): swapping guns doesn't skip a pump's delay.
    float ReadyTime = 0.0f;
};

// A healing or shield item (21.40's UFortAthenaAIBotEvaluator_Heal uses them), from its fire ability's blueprint
// defaults: GA_Athena_Bandage (MaxHealingAmount 15, HealingCap 75, TriggerDuration 3.5), GA_Athena_Medkit (HealingAmount
// 100, 10 s), GA_Athena_ShieldSmall (ShieldingAmount 25, SmallShieldCap 50, 2 s), GA_Athena_Shields (ShieldingAmount 50,
// 5 s), GA_Athena_SuperMedkit (HealingAmount 100, 15 s; the Chug Jug, "full health and shield").
struct FCoreAthenaBotConsumable
{
    UFortWeaponItemDefinition* ItemDefinition = nullptr;
    float Health = 0.0f;
    float HealthCap = 100.0f;
    float Shield = 0.0f;
    float ShieldCap = 100.0f;
    float UseTime = 2.0f;
    int32_t MaxStackSize = 1;
};

class FCoreAthenaBotController
{
public:
    FCoreAthenaBotController(FCoreAthenaBotManager* InManager, AAIController* InController, int32_t InBotIndex, uint8_t InTeam);

    AAIController* GetController() const;
    AFortPlayerPawnAthena* GetPawn() const;
    AFortPlayerStateAthena* GetPlayerState() const;
    uint8_t GetTeam() const { return Team; }
    // The bot's squad within its team (AFortPlayerStateAthena::SquadId), 0 for none: in a large team game (Teams of 20,
    // HasSquadsInTeams) a team is several squads, and the bot's teammates for its squad play are its squad's.
    uint8_t GetSquad() const { return Squad; }
    void SetSquad(uint8_t InSquad) { Squad = InSquad; }
    // A bot in an empty place on a player's team ("Fill players' teams with bots"), not on a team of its own; in a large
    // team game, in an empty place in a player's squad.
    bool IsOnPlayersTeam() const { return bOnPlayersTeam; }
    void SetOnPlayersTeam(bool bInOnPlayersTeam) { bOnPlayersTeam = bInOnPlayersTeam; }
    int32_t GetBotIndex() const { return BotIndex; }
    ECoreAthenaBotState GetState() const { return State; }
    bool IsAlive() const { return State != ECoreAthenaBotState::Dead; }
    const std::wstring& GetBotName() const { return BotName; }
    int32_t GetKills() const { return Kills; }
    // The enemy the bot is fighting, the opponent it is heading for (Investigate), and the POI it is travelling to (an
    // index into the manager's spots, or -1).
    AFortPawn* GetCurrentTarget() const { return CurrentTarget.Get(); }
    AFortPawn* GetHuntTarget() const { return Goal == ECoreAthenaBotGoal::Hunt ? HuntTarget.Get() : nullptr; }
    int32_t GetNextPOI() const { return NextPOI; }
    // What a landed bot is going for (teammates leave each other's to them, so they loot apart), and where it wanders to.
    const void* GetLootTarget() const { return LootTarget.Get(); }
    const void* GetContainerTarget() const { return ContainerTarget.Get(); }
    bool GetWanderGoal(FVector& OutGoal) const { OutGoal = WanderGoal; return bHasWanderGoal; }
    // The outfit the bot wears this match (an AthenaCharacter item: its hero gives the bot its class tags).
    UObject* GetLoadoutCharacter() const { return LoadoutCharacter; }

    void AssignDisplayName(const std::wstring& Name);
    void InitializeSkills(int32_t Level);
    // Spawns the warmup pawn at Location and possesses it.
    bool SpawnPawnAt(const FVector& Location, float Yaw, bool bSkydive);
    // The bus has left: the warmup pawn goes, the bot rides the aircraft until its jump.
    void EnterAircraft(AFortAthenaAircraft* InAircraft, float JumpTime, float LatestJumpTime);
    // The aircraft the bot rides: its team's where the match has one per team (50v50), else the only one.
    AActor* GetAircraft() const { return reinterpret_cast<AActor*>(BoardedAircraft.Get()); }
    // Leaves the match (a surplus bot, or the match ending): its pawn and controller are destroyed.
    void Destroy();

    void Tick(float Now, float DeltaSeconds);

    // The pawn died: Core's OnPossesedPawnDied (21.40's AFortAthenaAIBotController::OnPossesedPawnDied).
    void OnPossesedPawnDied(AFortPlayerStateAthena* KillerState);
    // Out of the match, a bot a player eliminated spectates that player, as an eliminated player's SpectateOnDeath does
    // (21.40's AFortAthenaAIBotController::OnBeginSpectating): its player state in theirs' Spectators, so their HUD counts
    // it. Bots have no client, so there's no camera: only the replicated state. It watches for 20 to 120 seconds, or
    // until that player is out, then stops (OnEndSpectating). A bot eliminated by a bot, the storm or a fall doesn't
    // spectate (CoreV2 #255).
    void OnBeginSpectating(AFortPlayerStateAthena* KillerState, float Now);
    void OnEndSpectating();
    bool IsSpectating() const { return SpectateEndTime >= 0.0f; }
    // A playlist that brings its players back (Playground's InfiniteRespawnExceptStorm, CoreV2 #344): the dead bot stays
    // in the match, as a player does (AFortGameModeAthena::IsRespawningAllowed), and comes back once Athena's respawn
    // countdown is over (Anthem_DeathPenalty's AthenaRespawnDelayOverTime: 10 s), skydiving from TeleportToSkyDive's
    // height over where it died, as Core respawns a player there (SpawnDefaultPawnFor_Implementation).
    bool IsRespawnPending() const { return RespawnTime >= 0.0f; }
    void RespawnPlayerAfterDeath(float Now);
    static constexpr float RespawnDelay = 10.0f;
    void AddKill();

    // The bus: where the bot lands (FCoreAthenaBotManager::AssignLandingSpots picks a named location or a
    // loot-rich spot near the bus's path) and when it jumps to get there.
    void SetLandingSpot(const FVector& Spot, float Radius, float InJumpTime);
    bool HasLandingSpot() const { return bHasLandingSpot; }
    // A bot on a player's team: its player teammate left the bus (a pawn at Where): the bot jumps after them and heads for
    // them while it falls (bLanded: they are on the ground).
    void FollowTeammateDrop(const FVector& Where, bool bLanded, float Now);
    bool IsFollowingTeammateDrop() const { return bFollowingDrop; }

    // ABuildingSMActor::AttemptSpawnResources with the bot's pawn as instigator: the materials its hit gave (a player's
    // go into the inventory; the bot keeps them, and drops them on death as a player does).
    void OnResourcesHarvested(ABuildingSMActor* Building, UFortItemDefinition* ResourceItem, int32_t Count, bool bDestroyed);
    int32_t GetMaterialCount() const { return Materials[0] + Materials[1] + Materials[2]; }

    // Squads (duos and squads playlists): Battle Royale's DBNO for a bot. A hit that would take it out while a teammate
    // is still standing downs it instead: a bot's hit through Core's damage (FCoreAthenaBotManager::ApplyBotDamage,
    // EnterDBNO: the pawn's bIsDBNO, its DBNO health and the game's GAB_AthenaDBNO), a player's through the game's own
    // DBNO handling (Tick notices it). It bleeds out, is finished off, or a teammate revives it.
    bool IsDBNO() const;
    bool CanGoDBNO() const;
    void EnterDBNO(AFortPlayerStateAthena* Downer, float Now);
    // What a bot does once down, whoever downed it; bHealed when the pawn has its DBNO health already.
    void OnEnteredDBNO(AFortPlayerStateAthena* Downer, float Now, bool bHealed);
    void OnRevived();
    int32_t GetRevives() const { return Revives; }
    int32_t GetTimesDowned() const { return TimesDowned; }

    // A healing or shield item's values, or null for anything else (cached per item).
    static const FCoreAthenaBotConsumable* GetConsumableInfo(UFortItemDefinition* Definition);
    // Loads what the bots use later (the emotes, their animations and the emote ability) up front, so no bot's first
    // use of one loads it in the middle of a match.
    static void PreloadAssets();
    // A new match (FCoreAthenaBotManager::Create): the last match's per-match state is dropped (the hot drop spot,
    // the weapons' fire cues and whether the game's fire works for them).
    static void ResetForNewMatch();
    // A gun's kind and stats row values (cached per weapon).
    static const FCoreAthenaBotWeapon& GetWeaponInfo(UFortWeaponItemDefinition* Definition);
    // Whether the bot wants this floor pickup: a gun it would carry (WeaponSlotFor), ammo for its guns (and some of the
    // other kinds while it has a free weapon slot), materials below a stack, or a consumable it has room for.
    bool WantsPickup(UFortItemDefinition* Definition, bool bWeapon, bool bConsumable);
    // 0 wood, 1 stone, 2 metal (UFortGameData's resource items), -1 for anything else.
    static int32_t MaterialKindOf(const UFortItemDefinition* Definition);

private:
    // The warmup island (Core's: 8.30's bots spawned after the bus). Players there stand around, look about, walk a
    // little, dance, and now and then pick up the island's weapons and fight; one eliminated comes back a few seconds
    // later at a warmup spot, which counts for nothing. A fall from a height opens the glider, as a player redeploys it
    // there (AFortPlayerPawnAthena::BeginSkydiving: the movement opens the parachute below its forced-deploy height).
    enum class EWarmupActivity : uint8_t
    {
        Idle,
        Wander,
        Emote,
        Brawl,
    };
    void TickWarmup(float Now, float DeltaSeconds);
    void ChooseWarmupActivity(float Now, bool bFightBack);
    bool WarmupGlide(float Now);
    void RespawnInWarmup(float Now);
    // What the bot carries goes (the bus leaving, a warmup respawn: a player's ResetToStartingItems).
    void ClearInventory();

    // 21.40's UFortAthenaAIBotEvaluator_JumpOffBus / _FreeFalling.
    void JumpOffBus(float Now);
    void FreeFalling(float Now);
    // On the bus (from 5.30), some bots thank the bus driver as a player does: AFortPlayerControllerAthena::
    // ServerThankBusDriver_Implementation's work on the bot's player state, which every client's feed shows.
    void ThankBusDriver();
    // 8.30's perception (OnPerceptionSensed): the nearest visible enemy, and the alert level.
    void UpdateCurrentTarget(float Now);
    // Whether the bot takes on another bot it sees (Core's: players come first, so a match lasts longer for them and
    // the kills are theirs): one that is shooting at it or going for it always; otherwise now and then.
    bool WillEngageBot(FCoreAthenaBotController* Other, float Now) const;
    // Whether the bot holds off finishing a knocked (DBNO) enemy for now, so a teammate gets a chance to revive it
    // (CoreV2 #233): true for a few seconds after the knock, longer while a squadmate is right there to revive; false in
    // the late game (few players left) or when no squadmate could revive it. The caller only defers while a standing
    // enemy is also in reach, so a lone downed enemy is still finished eventually.
    bool ShouldDeferThirst(AFortPawn* Downed, float Now) const;
    // Out of a fight: the goal the bot pursues (ChooseGoal picks a new one when the last is done), and PursueGoal's step
    // towards it, false when it is done or no longer applies.
    const wchar_t* ChooseGoal(float Now);
    bool PursueGoal(ECoreAthenaBotGoal InGoal, float Now);
    void SetGoal(ECoreAthenaBotGoal NewGoal, float Now);
    // 21.40's UFortAthenaAIBotEvaluator_Investigate: an armed bot with nothing left to loot heads for a player near it
    // (a few bots at most go for the same one), and for an enemy it just lost sight of.
    bool Investigate(float Now);
    // 21.40's UFortAthenaAIBotEvaluator_SelectNextPOI (FindNextBestPOI, SetNextDestination, ClearNextDestination): the
    // spot looted out, the next one nearby inside the safe zone (FCoreAthenaBotManager::FindNextBestPOI).
    bool SelectNextPOI(float Now);
    void ClearNextDestination();
    // 8.30's _RangeAttack and _ManageWeapon, 21.40's _MeleeAttack.
    bool RangeAttack(float Now);
    void ManageWeapon(float Now);
    // 21.40's _Loot (GatherNearestLoot): the nearest weapon or ammo on the floor, and taking it.
    bool Loot(float Now, float MaxDistance);
    // 8.30's _Storm: back inside the next safe zone.
    bool Storm(float Now);
    // Whether the safe zone comes before anything but a fight (CoreV2 #233: bots looted, healed or stood fighting until
    // the storm caught them, and died in it): in the storm already, or outside the next circle with no more time before
    // the storm finishes closing in than it takes to run there. A fight on the way is still taken, moving inwards.
    bool ShouldHeadForSafeZone(float Now) const;
    // 21.40's _Movement (TryToUnstuck) and a wander inside the safe zone.
    void Wander(float Now);
    void MoveTowards(const FVector& Destination, float Now, float AcceptanceRadius = 100.0f);
    void StopMoving();
    void TryToUnstuck(float Now);
    // 21.40's _Movement: a path on the invoker navmesh (AAIController::MoveToLocation with pathfinding), a far goal a leg
    // on the navmesh at a time, the next leg asked for before the last one ends. False when the bot steers instead, and
    // SteerGoal is where it steers to (the goal, or its nearest navmesh point when it is just off the navmesh).
    bool FollowPath(const FVector& Destination, float Now, float AcceptanceRadius, FVector& SteerGoal);
    bool RequestPath(const FVector& Destination, float Now, float AcceptanceRadius, FVector& SteerGoal);
    // The last resort, without a path: straight at the goal, around what blocks the way (traces ahead at knee and head
    // height across the capsule's width: jump a low obstacle, turn to the side that opens up from a wall).
    void SteerAroundObstacles(FVector& Direction, float Now);
    // A closed door right ahead (on the path or the steered heading) is opened with the wall's own interaction, as a
    // player's interact does.
    void OpenDoorInTheWay(const FVector& Direction, float Now);
    // The pawn's EFortMovementStyle: a player sprints to get somewhere and runs while fighting (AFortPawn's
    // GetMovementSpeed reads the movement set's speed for the style; an AI controller never sets it).
    void SetMovementStyle(bool bSprint, float Now);
    // Where the bot looks. AAIController::UpdateControlRotation turns the pawn to its focal point at once, so the bot
    // turns there itself at a player's turn rate: the point it wants to look at (FaceTowards: a target, a chest, a
    // teammate) for a moment, else the way it is moving; each frame the focal point is the turned heading.
    void FaceTowards(const FVector& Point, float Now, float Hold = 0.4f);
    void UpdateFacing(float Now, float DeltaSeconds);
    // How far (degrees) the bot's heading is from the point it wants to look at.
    float FacingError() const;
    // The target is out of sight behind a player's builds right around it (it boxed itself in).
    bool IsTargetBoxedIn(AFortPawn* Target) const;
    void LogMovementDiagnostics();
    // 21.40's _Loot for containers: walk to the nearest unsearched chest or ammo box and search it (its own
    // ServerOnAttemptInteract), then take what comes out. A new one is looked for within MaxDistance.
    bool SearchContainer(float Now, float MaxDistance = 5000.0f);
    // The storm hurts a bot outside its circle as it does players (Core's, when the game's own storm damage doesn't
    // reach the bot's pawn).
    void TakeStormDamage(float Now);

    bool HasUsableWeapon() const;
    bool HasAmmoFor(const FCoreAthenaBotWeapon& Weapon) const;
    int32_t BestWeaponIndex() const;
    // 8.30's _ManageWeapon: the weapon whose EquipMaxDistance suits a target this far (a shotgun close, an assault
    // rifle or SMG at mid range, a sniper far, a pistol when nothing better has ammo).
    int32_t WeaponIndexForRange(float Distance) const;
    // Where a weapon found on the floor would go: -1 a free slot, an index the weapon it replaces, -2 not wanted.
    int32_t WeaponSlotFor(UFortWeaponItemDefinition* Definition) const;
    void EquipWeapon(int32_t Index);
    // The held weapon's reload: its stats row's ReloadTime (a share of it for a partial individual-bullet reload).
    void StartReload(float Now);
    void CancelReload();
    // In a fight: an enemy in sight, or hurt in the last few seconds (outside the storm).
    bool IsInCombat(float Now) const;
    // An enemy close by in sight (20 m), or hurt in the last 2 s: what stops a revive.
    bool IsUnderThreat(float Now) const;
    // 21.40's UFortAthenaAIBotEvaluator_Heal: out of combat, a bandage, medkit, shield potion or Chug Jug the bot
    // carries, held for the item's use time. True while the bot is using one.
    bool Heal(float Now);
    void CancelHeal();
    // An emote, as a player's emote wheel plays it (AFortPlayerController::ServerPlayEmoteItem: the game data's
    // EmoteGameplayAbility given to the pawn's ability system with the emote as its source, activated once). Bots dance
    // on the warmup island (the community list) and after an elimination when nobody is shooting at them (21.40's
    // UFortAthenaAIBotEvaluator_DanceOnKill).
    bool PlayEmote(float Now, float Duration);
    // Ends the emote the bot is playing (a player's emote ends as they move): its emote ability's instance ends.
    void StopEmote();
    // A boogie bomb's dance (GA_DanceGrenade_Stun, which the grenade activates by its ability tag on the pawns it hits):
    // the bot stands, dancing, until it ends.
    bool IsDanceStunned() const;
    // Materials (the community list: bots gather materials and drop them on death): out of a fight with few materials,
    // the bot swings its pickaxe at a tree, rock or wall next to it, and a stuck bot breaks what is in its way. The
    // pickaxe's own ability refuses a bot, so the hit is the pickaxe's damage effect (GE_Melee_GenericDamage) applied by
    // the bot's ability system to the building's, with the pickaxe as the context's damage source, and the materials
    // come from the building's own resource spawn (ABuildingSMActor::AttemptSpawnResources -> OnResourcesHarvested).
    bool Harvest(float Now);
    void StopHarvesting();
    bool SwingAtBuilding(ABuildingSMActor* Building, float& OutDamage);
    // A building piece hit by the bot's weapon (a pickaxe swing, or a shot a wall stopped): the weapon's damage effect
    // with the weapon as its damage source, so the building takes what the weapon's stats give it.
    bool DamageBuilding(ABuildingSMActor* Building, AFortWeapon* Weapon, float& OutDamage);
    // A pawn hit by the bot's weapon, the game's way (its damage effect, Hits times for a shotgun's pellets).
    bool DamagePawn(AFortPawn* Victim, AFortWeapon* Weapon, int32_t Hits, float& OutDamage);
    // The weapon's damage effect (GE_Ranged_GenericDamage, or the pickaxe's GE_Melee_GenericDamage) with the weapon as the
    // context's damage source, from the bot's ability system to the target's.
    // HitFrom: where the shot came from. The context then carries the hit as the weapon's ability adds it (its hit result,
    // from the shooter to the target, and its origin), which the victim's damage cue shows as the hit's direction.
    bool ApplyWeaponDamageEffect(UObject* TargetActor, AFortWeapon* Weapon, bool bMelee, const FVector* HitFrom = nullptr);
    void EquipPickaxe();
    // A swing or a shot as players see it: the held weapon's fire ability's montage (UFortGameplayAbility::MontageInfo:
    // the pickaxe's PlayerMeleeCombo, a gun's fire), replicated as the reload's is.
    void PlayFireMontage();
    // The playing montage ends after its current section (a melee combo's sections loop into each other).
    void EndMontageAfterSection(UObject* Montage);
    // A montage shown to the players (reload, healing), as an ability's PlayMontageAndWait replicates it: the pawn's
    // PlayAnimMontage (its ability system's PlayMontage), or the ability system's RepAnimMontageInfo (and the Athena
    // pawn's copy) where that doesn't replicate. bStop stops the current one.
    bool PlayReplicatedMontage(UObject* Montage, float PlayRate, bool bStop = false);
    // Squads: a downed bot's tick (bleeding, crawling to a teammate, eliminated with its team), the revive of a downed
    // teammate, and staying near the team. A bot on a player's team counts its player teammates too.
    void Downed(float Now);
    bool ReviveTeammate(float Now);
    // A player's revive as the game runs it on the server: the interaction's start (AFortPlayerControllerAthena::
    // ServerDBNOReviveStarted) puts the downed pawn in its "being revived" state, and its end (ServerDBNOReviveInterrupted,
    // also what the Athena revive does once the pawn is up) takes it out again.
    void BeginReviveInteraction(AFortPlayerPawnAthena* Downed);
    void EndReviveInteraction();
    bool IsReviving() const { return ReviveDoneAt > 0.0f; }
    bool StayWithTeam(float Now);
    // A bot in a player's squad goes to the map marker one of its players placed (FCoreAthenaBotManager::
    // GetSquadMapMarker), out of a fight, healing, reviving and the storm, takes what lies close by there, then plays on
    // from there until the marker is moved. One in the storm is left until the circle reaches it.
    bool FollowMapMarker(float Now);
    // TryToUnstuck's ways out, as a stuck player gets out: the piece in the way broken with the pickaxe, the floor under
    // it broken to get down, a ramp built up a hill or a cliff or out of a basement (Chapter 2's bots), the ceiling
    // broken to make room for it. False when none applies.
    bool FreeItself(float Now, const wchar_t* Why);
    // The way the bot is trying to go: along its path (the path's next point), else towards where it steers, else its goal.
    FVector WayAhead(float Now, const FVector& Location) const;
    // TryToUnstuck: a building the bot can break right in front of it, on the way to its goal.
    bool FindBuildingInTheWay(float Now);
    // A ramp (and a second one up from its top where the hill goes on) towards the way it is trying to go, at most every
    // few seconds and a few times a minute.
    bool BuildWayUp(float Now, const FVector& Location);
    // A building's piece over its head (a basement's ceiling, a floor above) broken, to get up through it.
    bool BreakUpThroughCeiling(float Now, const FVector& Location);
    // A bot up on a building (a roof, an upper floor) with where it is going well below it and no way down: the piece it
    // stands on, broken with the pickaxe (Harvest), as a player gets down into a house (CoreV2 #275).
    bool BreakDownFromRoof(float Now, const FVector& Below);
    // Chapter 2's bot building (21.40), for builds before 8.30. Nothing is built on the warmup island, by a bot that is
    // down or not on its feet, or on 1.1-1.8 (their own building RPC bodies leave the initialize indices unresolved).
    // UFortAthenaAIBotEvaluator_DefensiveBuilding: hit by an enemy it can see, out in the open, now and then one of the
    // skill set's WeightedBuildingLists towards it (a wall, or a wall with a ramp up to it), then
    // DefensiveBuildingDelayBetweenBuilds before the next.
    bool DefensiveBuilding(float Now, AFortPawn* Attacker);
    // UFortAthenaAIBotEvaluator_Bunker (UFortAthenaAIPerk_Bunker): a 1x1 box of walls around the bot as it heals, or when
    // an enemy it hadn't seen shoots it (OddsToBeUsedAgainstUnknownThreat), with a cone roof (OddsToUseRoofBuilding) and a
    // ramp inside towards the threat (OddsToBuildForwardRamp). Threat may be null (the way the bot faces then).
    bool Bunker(float Now, AFortPawn* Threat, bool bUnknownThreat);
    // Starts a build: the origin cell is the bot's, forward the grid axis nearest the way to Towards. Pieces the bot can't
    // afford or whose place isn't clear are left out; false when none is left.
    bool StartBuildingQueue(float Now, const FVector& Towards, const std::vector<FCoreAthenaBotBuildDescriptor>& Descriptors, float InitialDelay, float SameDelay, float SameDeviation,
        float DifferentDelay, float DifferentDeviation, const wchar_t* Reason, bool bToUnstick = false);
    // UFortAthenaAIBotBuildingComponent's tick: the next piece once its delay is up. True while a build is under way (the
    // bot stands and holds fire: UFortAthenaBTTask_Build's BlockWeaponActions).
    bool TickBuildingQueue(float Now);
    void ClearBuildingQueue();
    // Where a piece goes (the actor's origin and yaw) and whether its place is clear of the world and of anyone's pieces.
    bool GetPiecePlacement(const FCoreAthenaBotBuildDescriptor& Descriptor, FVector& OutLocation, float& OutYaw) const;
    bool IsPiecePlaceClear(const FCoreAthenaBotBuildDescriptor& Descriptor) const;
    // A player piece of the queue's material spawned and initialized as ServerCreateBuildingActor's body does after its
    // checks, with the bot's team; its materials paid.
    bool BuildPiece(const FCoreAthenaBotBuildDescriptor& Descriptor);
    void TakePickup(AFortPickup* Pickup);
    // An item dropped where the bot stands, as a player drops it (a weapon with its magazine). A death drop is tossed onto
    // DeathSpot, its spot around the body (AFortPlayerControllerAthena::GetInventoryDropSpots).
    void DropItem(UFortItemDefinition* Definition, int32_t Count, int32_t LoadedAmmo, const FVector* DeathSpot = nullptr);
    void DropInventory();
    void ApplyStartingHealth();
    // The bot stops shooting: the shot's fire input is let go (EndShot) and a held fire cue goes (RemoveWeaponFireCue).
    void StopFiring();
    // One shot's fire input let go (the pawn's PawnStopFire, the game's fire ability's input release). A gun held
    // firing keeps its fire cue between shots.
    void EndShot();
    // Aiming down sights as a player does with a gun in a fight: the pawn's replicated bIsTargeting (AFortPlayerPawn,
    // OnRep_IsTargeting), which other clients' animation reads for the aiming pose.
    void SetTargeting(bool bNewTargeting);
    // One shot at the target: the weapon's fire for everyone to see, the magazine, and its hit, by Core (no player
    // controller aims a bot's weapon ability, so the game's own fire neither spends ammo nor hits).
    void FireShot(AFortPawn* Target, float Distance);
    // The weapon's own fire: its primary fire ability (PrimaryAbilitySpecHandle), activated on the bot's ability system
    // as a player's input does, so the shot, its ammo, its hit and its effects are the game's. False when it didn't
    // activate (Core's shot is used then).
    bool ActivateWeaponFire(AFortWeapon* Weapon);
    void ReleaseWeaponFire();
    // Core's shot, shown to the players: the weapon's fire cue (its fire ability's WeaponFireCue,
    // GameplayCue.Weapons.Activation) executed through the pawn's NetMulticast_InvokeGameplayCueExecuted_WithParams,
    // as the game's own fire ability sends it, with the weapon as its causer and the shot's end as its location.
    void PlayWeaponFireCue(AFortWeapon* Weapon, const FVector& ImpactLocation);
    // A gun with persistent fire effects (AFortWeaponRanged::bPersistentFireFX: the automatic ones) has its fire cue added
    // while the trigger is held and removed as it's let go, as UFortGameplayAbility_RangedWeapon does: its activation
    // adds each WeaponFireCue tag to the ability system's ActiveGameplayCues (3.3 0x85CE00), its EndAbility removes them
    // (0x867450). Clients start the weapon's looping fire effects on the cue's WhileActive and stop them on its Removed
    // (AFortWeapon::GameplayCue_Weapons_Activation, 0xFC5EB0: PlayWeaponFireFX, StopWeaponFireFX).
    bool AddWeaponFireCue(AFortWeapon* Weapon, FName Tag, const FVector& ImpactLocation);
    void RemoveWeaponFireCue();
    // Whether the game's fire works for this weapon class: unknown until a shot shows it spending ammo or not.
    void CheckWeaponFireResult(float Now);

    FCoreAthenaBotManager* Manager = nullptr;
    TWeakObjectPtr<AAIController> Controller;
    TWeakObjectPtr<AFortPlayerPawnAthena> PlayerBotPawn;
    int32_t BotIndex = 0;
    uint8_t Team = 0;
    uint8_t Squad = 0;
    bool bOnPlayersTeam = false;
    // Following a player teammate's jump from the bus (FollowTeammateDrop).
    bool bFollowingDrop = false;
    std::wstring BotName;
    ECoreAthenaBotState State = ECoreAthenaBotState::Warmup;

    FCoreAIBotSkills AIBotSkills;
    ECoreAlertLevel CurrentAlertLevel = ECoreAlertLevel::Unaware;
    TWeakObjectPtr<AFortPawn> CurrentTarget;
    bool bHasVisualLineOfSightOnTarget = false;
    FVector LastKnownPosition;

    std::vector<FCoreAthenaBotWeapon> WeaponList;
    std::map<UFortItemDefinition*, int32_t> Ammo;
    // Healing and shield items carried, and the one in use.
    std::map<UFortItemDefinition*, int32_t> Consumables;
    UFortItemDefinition* HealingItem = nullptr;
    float HealUntil = 0.0f;
    float NextHealCheck = 0.0f;
    int32_t Heals = 0;
    // Reload and weapon choice: the weapon being reloaded, the last weapon change, and when the bot was last hurt.
    int32_t ReloadIndex = -1;
    float NextWeaponSwitch = 0.0f;
    float LastHurtTime = -100.0f;
    float LastHealthShield = -1.0f;
    int32_t Reloads = 0;
    int32_t WeaponSwitches = 0;
    // Emotes and strafing: when the current emote ends, the next warmup emote, a dance owed for a kill, and the strafe.
    float EmoteUntil = 0.0f;
    float NextEmoteTime = 0.0f;
    bool bDanceOnKill = false;
    int32_t Emotes = 0;
    float StrafeSign = 1.0f;
    float NextStrafeChange = 0.0f;
    // Harvesting: the building being hit, and the materials carried (wood, stone, metal).
    TWeakObjectPtr<ABuildingSMActor> HarvestTarget;
    float HarvestTargetTime = 0.0f;
    float NextHarvestSwing = 0.0f;
    float NextHarvestSearch = 0.0f;
    FVector HarvestPoint;
    bool bHarvestToUnstick = false;
    // Set when the building's resource spawn credited the bot during a swing.
    bool bHarvestCredited = false;
    int32_t Materials[3] = {};
    // DefensiveBuilding's CanBuildNextTimeDelay and the bunker perk's cooldown: no build of that kind before these times.
    float NextDefensiveBuildTime = 0.0f;
    float NextBunkerTime = 0.0f;
    // UFortAthenaAIBotBuildingComponent: the build under way (BuildQueue), the next piece's index, when the last piece went
    // up (LastBuildTime) and the wait before the next (DelayBeforeNextBuildPiece).
    FCoreAthenaBotBuildingQueue BuildQueue;
    size_t NextBuildPiece = 0;
    float LastBuildTime = 0.0f;
    float DelayBeforeNextBuildPiece = 0.0f;
    // The bunker's HideEndTime: the bot stays in its box until then (unless the storm comes).
    float HideEndTime = 0.0f;
    int32_t PiecesBuilt = 0;
    int32_t BuildsStarted = 0;
    int32_t Swings = 0;
    int32_t BreakThroughs = 0;
    // Squads: who downed the bot, when, its bleed, and the teammate it is reviving.
    // Out of the match: the player it spectates, and when it stops (-1: not spectating).
    TWeakObjectPtr<AFortPlayerStateAthena> SpectatedPlayer;
    float SpectateEndTime = -1.0f;
    float NextSpectateCheckTime = 0.0f;
    void TickSpectating(float Now);
    TWeakObjectPtr<AFortPlayerStateAthena> Downer;
    float DBNOStartedAt = 0.0f;
    // The pawn has had its DBNO health (a pawn the game downed is at 0 until the DBNO ability heals it).
    bool bDBNOHealed = false;
    float NextBleedTime = 0.0f;
    float LastDBNOHealth = 0.0f;
    FCoreAthenaBotController* ReviveTarget = nullptr;
    // A downed player teammate the bot is reviving (a bot on a player's team), instead of ReviveTarget.
    TWeakObjectPtr<AFortPlayerPawnAthena> RevivePlayer;
    float ReviveDoneAt = 0.0f;
    // The downed pawn whose "being revived" state the bot's revive started (BeginReviveInteraction).
    TWeakObjectPtr<AFortPlayerPawnAthena> ReviveInteractionPawn;
    int32_t Revives = 0;
    int32_t TimesDowned = 0;
    // Shots on target that a wall or a build stopped.
    int32_t BlockedShots = 0;
    // The landing spot chosen on the bus.
    bool bHasLandingSpot = false;
    FVector LandingSpot;
    float LandingRadius = 0.0f;
    int32_t EquippedIndex = -1;
    TWeakObjectPtr<AFortWeapon> EquippedWeapon;
    bool bPickaxeEquipped = false;
    int32_t Kills = 0;
    int32_t ShotsFired = 0;

    FWeakObjectPtr BoardedAircraft;

    // Timers (world seconds).
    float JumpTime = 0.0f;
    float LatestJumpTime = 0.0f;
    // When the bot thanks the bus driver: -2 until its first tick on the bus picks it, -1 for not at all.
    float ThankBusDriverTime = -1.0f;
    float NextPerceptionTime = 0.0f;
    float NextDeathCheck = 0.0f;
    float NextStormTick = 0.0f;
    float LastStormHealth = -1.0f;
    float NextLootSearch = 0.0f;
    float NextWanderTime = 0.0f;
    float NextShotTime = 0.0f;
    float FireUntil = 0.0f;
    bool bFiring = false;
    // The fire cue Core added to the bot's ability system for a gun with persistent fire effects (AddWeaponFireCue), and
    // until when the trigger counts as held: the next shot due by then keeps it.
    FName FireCueTag;
    TWeakObjectPtr<UAbilitySystemComponent> FireCueAbilitySystem;
    float FireCueUntil = 0.0f;
    bool bTargeting = false;
    bool bBoxedInLogged = false;
    // ShouldHeadForSafeZone this tick.
    bool bHeadingForSafeZone = false;
    // The time of the bot's last tick, and when a gun just taken out can first fire (its equip, as a player waits it out).
    float LastTickTime = 0.0f;
    float EquipReadyTime = 0.0f;
    // The pickaxe of the bot's loadout (a random one of the build's), or null for the default.
    UAthenaPickaxeItemDefinition* LoadoutPickaxe = nullptr;
    // The back bling of the bot's loadout (an AthenaBackpack item: its character parts), or null.
    UObject* BackpackDefinition = nullptr;
    // The rest of the loadout, chosen once (the warmup's pawn and the one that leaves the bus look the same).
    UObject* LoadoutCharacter = nullptr;
    UObject* LoadoutGlider = nullptr;
    UObject* LoadoutContrail = nullptr;
    bool bLoadoutChosen = false;
    float ReloadUntil = 0.0f;
    float TargetAcquiredTime = 0.0f;
    float NextStuckCheck = 0.0f;
    // When the bot was first seen held up against a wall on its path (-1: it isn't).
    float HeldUpOnPathSince = -1.0f;
    float LandedTime = 0.0f;
    float NextLogTime = 0.0f;
    float WarmupRespawnTime = -1.0f;
    // When a dead bot's respawn is due (-1: none pending, RespawnPlayerAfterDeath).
    float RespawnTime = -1.0f;
    EWarmupActivity WarmupActivity = EWarmupActivity::Idle;
    float WarmupActivityUntil = 0.0f;
    float NextLookAroundTime = 0.0f;
    float NextGlideCheck = 0.0f;
    bool bWarmupGliding = false;
    // A new warmup pawn stands still for a few seconds before the warmup's activities begin.
    bool bWarmupArrivalHold = false;
    int32_t WarmupDeaths = 0;
    int32_t WarmupGlides = 0;
    int32_t WarmupBrawls = 0;
    FVector LastStuckCheckLocation;
    int32_t StuckChecks = 0;
    // TryToUnstuck's last give-up (a second one near it gives up the far goal too), and the move goal it measures
    // progress towards: how near it got and since when.
    float LastGiveUpTime = -100.0f;
    FVector LastGiveUpLocation;
    FVector ProgressGoal;
    float ProgressDistance = -1.0f;
    float ProgressSince = 0.0f;
    // Where the bot has been held, within a couple of metres, since when (-1: it is moving on): twitching back and forth
    // against a wall moves it more than the 60 cm check between two looks, and isn't caught by that.
    FVector ConfineAnchor;
    float ConfineSince = -1.0f;
    // Its unstick builds: none before NextUnstickBuildTime, and at most a few in a minute (from UnstickBuildWindowStart).
    float NextUnstickBuildTime = 0.0f;
    float UnstickBuildWindowStart = -100.0f;
    int32_t UnstickBuildsInWindow = 0;
    int32_t Climbs = 0;

    FVector DropTarget;
    FVector MoveGoal;
    bool bHasMoveGoal = false;
    TWeakObjectPtr<AFortPickup> LootTarget;
    TWeakObjectPtr<ABuildingContainer> ContainerTarget;
    // Loot and containers the bot gave up on (out of reach: above or below it, or it got stuck on the way).
    std::vector<const void*> Unreachable;
    // When the bot set off for its loot or container target: one it hasn't reached in a while is out of reach.
    float LootTargetTime = 0.0f;
    float LastLootPursuit = 0.0f;
    float ContainerTargetTime = 0.0f;
    float LastContainerPursuit = 0.0f;
    bool IsUnreachable(const void* Object) const;
    void MarkUnreachable(const void* Object);
    // A pickup or container a bot teammate is going for: the squad loots apart instead of all going for one (CoreV2 #275).
    bool IsTakenByTeammate(const void* Object) const;
    // A bot on a player's team stays within reach of them: what it loots on its own lies within this of its nearest
    // player standing (StayWithTeam calls it back past 25 m once it is done). False with no player to stay by.
    bool GetSquadLeash(FVector& OutAround, float& OutRadius) const;
    float SearchUntil = 0.0f;
    float NextContainerSearch = 0.0f;
    int32_t ContainersSearched = 0;

    // Movement: the path being followed, the obstacle turn, and the logged diagnostics.
    bool bPathFollowing = false;
    FVector PathGoal;
    float NextPathRequest = 0.0f;
    float PathFailedUntil = 0.0f;
    int32_t PathRequests = 0;
    int32_t PathFailures = 0;
    // The path's end on the navmesh (a leg's, for a far goal), where the bot asked for it and when.
    bool bOnLeg = false;
    FVector PathTarget;
    FVector PathRequestLocation;
    float LastPathRequest = -100.0f;
    // Why the bot steers rather than follows a path, for the status line.
    const wchar_t* SteerReason = L"no goal";
    // Where it steers to meanwhile, when not its goal (its nearest navmesh point, just off the navmesh).
    FVector SteerTarget;
    bool bHasSteerTarget = false;
    bool bRegisteredInvoker = false;
    float NextObstacleProbe = 0.0f;
    float AvoidUntil = 0.0f;
    FVector AvoidDirection;
    // The side (1 left, -1 right, 0 none yet) the bot last turned to around an obstacle: it keeps to it.
    float AvoidSide = 0.0f;
    float NextDoorProbe = 0.0f;
    // A door whose interaction refused the bot, left alone for a while.
    const void* RefusedDoor = nullptr;
    float RefusedDoorUntil = 0.0f;
    int32_t Jumps = 0;
    float NextJumpTime = 0.0f;
    int32_t DoorsOpened = 0;
    bool bLoggedMovement = false;
    uint8_t MovementStyle = 0xFF;
    float NextMovementStyleChange = 0.0f;
    // The player state of whoever hurt the pawn last (from its Damagers when its health fell), so the elimination is
    // credited even when the pawn is gone by the time the bot sees its death.
    TWeakObjectPtr<AFortPlayerStateAthena> LastAttacker;
    float LastAttackerTime = -100.0f;
    // Whether the pawn was moving last frame (a pawn that starts moving is sent to clients at once).
    bool bWasMoving = false;
    bool bEmoting = false;
    bool bDanceStunned = false;
    float LastTargetInSightTime = -100.0f;
    // Facing (UpdateFacing): the point to look at, until when it outranks the way the bot moves, and the heading.
    FVector FacePoint;
    float FacePointUntil = 0.0f;
    bool bHasFacePoint = false;
    bool bHasFacingYaw = false;
    float FacingYaw = 0.0f;
    // Strafing input eased between directions (StrafeSign is where it heads).
    float StrafeAmount = 0.0f;
    float LastStrafeTime = 0.0f;
    // Skydiving: diving (the skydive's active control), and the heading it dives along.
    bool bDiving = false;
    FVector DiveHeading;

    // The game's own fire (ActivateWeaponFire): the shot waiting to show whether it spent ammo.
    bool bWeaponFireActive = false;
    float WeaponFireCheckTime = 0.0f;
    int32_t WeaponFireAmmoBefore = 0;
    int32_t NativeShots = 0;
    FVector WanderGoal;
    bool bHasWanderGoal = false;
    FVector AimError;
    // Tracking (21.40's TrackingInterpTime): where the bot's aim follows the target, lagging behind a moving one, and the
    // target it tracks. A target it just saw again is reacted to anew (TrackingReactionTime).
    FVector TrackedTargetLocation;
    TWeakObjectPtr<AFortPawn> TrackedTarget;
    float LastTrackTime = 0.0f;

    // The goal out of a fight (ChooseGoal), since when, and how often it changed (the 20 s log).
    ECoreAthenaBotGoal Goal = ECoreAthenaBotGoal::None;
    float GoalStartTime = 0.0f;
    int32_t GoalChanges = 0;
    // A stuck bot's detour (TryToUnstuck) before it sets off again for somewhere far.
    float DetourUntil = 0.0f;
    // Investigate: the player the bot heads for, when it looks again, and the enemy it last saw (and where).
    TWeakObjectPtr<AFortPawn> HuntTarget;
    float NextHuntSearch = 0.0f;
    TWeakObjectPtr<AFortPawn> LastSeenTarget;
    FVector LastSeenTargetLocation;
    int32_t Hunts = 0;
    // SelectNextPOI: the spots the bot has been to or looted, the one it heads for, where on the ground, and when it
    // looks for the next.
    std::vector<int32_t> VisitedPOIs;
    int32_t NextPOI = -1;
    FVector POIDestination;
    float NextPOISearch = 0.0f;
    float POITravelStartTime = 0.0f;
    int32_t POIsVisited = 0;
    // The bots it has let be, for the log.
    int32_t BotsIgnored = 0;
    // FollowMapMarker: the squad's marker it heads for (on the ground there), when it looks again, and how many it went to.
    // Reached: it got there and took what lay around it, and plays on (until the marker is moved).
    bool bHasMapMarker = false;
    bool bMapMarkerReached = false;
    FVector MapMarker;
    FVector MapMarkerGround;
    float NextMapMarkerCheck = 0.0f;
    int32_t MapMarkersFollowed = 0;
};
