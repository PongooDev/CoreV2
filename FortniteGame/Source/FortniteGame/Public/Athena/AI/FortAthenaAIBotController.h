#pragma once

#include "Math/Vector.h"
#include "Misc/Guid.h"
#include "UObject/WeakObjectPtr.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

class AAIController;
class AActor;
class ABuildingContainer;
class AFortGameModeAthena;
class AFortPawn;
class AFortPickup;
class AFortPlayerPawnAthena;
class AFortPlayerStateAthena;
class AFortWeapon;
class UFortItemDefinition;
class UFortWeaponItemDefinition;
class UFortServerBotManagerAthena;

// Core's backport of Epic's Battle Royale player bots (CoreV2 #233, Core-Docs Athena-Bots-Plan.md). Epic's
// AFortAthenaAIBotController (8.30, CL-5793395) is a UClass deriving from AAIController; 3.0 has none of the Athena AI
// classes and Core can't add UClasses, so each bot is a stock AAIController (with a player state) possessing an ordinary
// AFortPlayerPawnAthena, and this C++ class, named after Epic's, is its "brain": 8.30's controller fields
// (CurrentTarget, CurrentAlertLevel, WeaponList, Inventory, AIBotSkills) and its behaviour tree's evaluators, as
// functions named after 8.30's and 21.40's UFortAthenaAIBotEvaluator_* classes, run from Core's game-thread tick.
// None of 8.30's tuning ships (its behaviour tree, skill values and spawn data are server-only content), so the numbers
// here are Core's own 🟡.

// 8.30's FAIBotSkills (0x14 bytes: aim timing and tracking), which a bot's controller blueprint sets; Core fills it from
// the admin panel's bot level (UFortServerBotManagerAthena::SkillsForLevel), with the sight range and aim error 21.40
// keeps in its Perception and Aiming skill sets.
struct FAIBotSkills
{
    float MaxTimeBetweenShots = 0.4f;
    float DeviationTimeBetweenShots = 0.2f;
    float MaxTrackingReactionTime = 0.6f;
    float MaxTrackingInterpTime = 0.3f;
    float MaxTrackingPredictionError = 150.0f;
    // Core's: how far the bot sees an enemy, and how far off (in units, at 10 m) its aim strays.
    float SightRange = 6000.0f;
    float AimErrorAt10m = 60.0f;
};

// 8.30's EAlertLevel (the controller's CurrentAlertLevel, OnPerceptionSensed's state machine).
enum class EAlertLevel : uint8_t
{
    Unaware,
    Suspicious,
    Alerted,
    Threatened,
};

// Where a bot is in the match (Core's: 8.30's bots only ever spawned on the ground).
enum class EFortAthenaAIBotState : uint8_t
{
    Warmup,
    InAircraft,
    Skydiving,
    Landed,
    Dead,
};

// One of the bot's weapons (8.30's FFortAthenaInventoryInformation is the weapon list's entry; the item itself lives
// in the controller's own AFortInventory there, in Core's brain here).
struct FFortAthenaAIBotWeapon
{
    UFortWeaponItemDefinition* WeaponDefinition = nullptr;
    UFortItemDefinition* AmmoDefinition = nullptr;
    FGuid ItemGuid;
    int32_t LoadedAmmo = 0;
    int32_t ClipSize = 0;
    // From its stats row (FFortBaseWeaponStats::DmgPB, FFortRangedWeaponStats::BulletsPerCartridge and FiringRate).
    float Damage = 20.0f;
    int32_t Pellets = 1;
    float FiringRate = 2.0f;
};

class AFortAthenaAIBotController
{
public:
    AFortAthenaAIBotController(UFortServerBotManagerAthena* InManager, AAIController* InController, int32_t InBotIndex, uint8_t InTeam);

    AAIController* GetController() const;
    AFortPlayerPawnAthena* GetPawn() const;
    AFortPlayerStateAthena* GetPlayerState() const;
    uint8_t GetTeam() const { return Team; }
    int32_t GetBotIndex() const { return BotIndex; }
    EFortAthenaAIBotState GetState() const { return State; }
    bool IsAlive() const { return State != EFortAthenaAIBotState::Dead; }
    const std::wstring& GetBotName() const { return BotName; }
    int32_t GetKills() const { return Kills; }

    void AssignDisplayName(const std::wstring& Name);
    void InitializeSkills(int32_t Level);
    // Spawns the warmup pawn at Location and possesses it.
    bool SpawnPawnAt(const FVector& Location, float Yaw, bool bSkydive);
    // The bus has left: the warmup pawn goes, the bot rides the aircraft until its jump.
    void EnterAircraft(float JumpTime, float LatestJumpTime);
    // Leaves the match (a surplus bot, or the match ending): its pawn and controller are destroyed.
    void Destroy();

    void Tick(float Now, float DeltaSeconds);

    // The pawn died: Core's OnPossesedPawnDied (21.40's AFortAthenaAIBotController::OnPossesedPawnDied).
    void OnPossesedPawnDied(AFortPlayerStateAthena* KillerState);
    void AddKill();

private:
    // 21.40's UFortAthenaAIBotEvaluator_JumpOffBus / _FreeFalling.
    void JumpOffBus(float Now);
    void FreeFalling(float Now);
    // 8.30's perception (OnPerceptionSensed): the nearest visible enemy, and the alert level.
    void UpdateCurrentTarget(float Now);
    // 8.30's _RangeAttack and _ManageWeapon, 21.40's _MeleeAttack.
    bool RangeAttack(float Now);
    void ManageWeapon(float Now);
    // 21.40's _Loot (GatherNearestLoot): the nearest weapon or ammo on the floor, and taking it.
    bool Loot(float Now, float MaxDistance);
    // 8.30's _Storm: back inside the next safe zone.
    bool Storm(float Now);
    // 21.40's _Movement (TryToUnstuck) and a wander inside the safe zone.
    void Wander(float Now);
    void MoveTowards(const FVector& Destination, float Now, float AcceptanceRadius = 100.0f);
    void StopMoving();
    void TryToUnstuck(float Now);
    // 21.40's _Movement: a path on the invoker navmesh (AAIController::MoveToLocation with pathfinding), else steering
    // straight at the goal, around what blocks the way (traces ahead at knee and head height: jump a low obstacle,
    // turn aside from a wall).
    bool FollowPath(const FVector& Destination, float Now, float AcceptanceRadius);
    void SteerAroundObstacles(FVector& Direction, float Now);
    // The pawn's EFortMovementStyle: a player sprints to get somewhere and runs while fighting (AFortPawn's
    // GetMovementSpeed reads the movement set's speed for the style; an AI controller never sets it).
    void SetMovementStyle(bool bSprint);
    void LogMovementDiagnostics();
    // 21.40's _Loot for containers: walk to the nearest unsearched chest or ammo box and search it (its own
    // ServerOnAttemptInteract), then take what comes out.
    bool SearchContainer(float Now);
    // The storm hurts a bot outside its circle as it does players (Core's, when the game's own storm damage doesn't
    // reach the bot's pawn).
    void TakeStormDamage(float Now);

    bool HasUsableWeapon() const;
    int32_t BestWeaponIndex() const;
    void EquipWeapon(int32_t Index);
    void TakePickup(AFortPickup* Pickup);
    void DropInventory();
    void ApplyStartingHealth();
    void StopFiring();
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
    // Whether the game's fire works for this weapon class: unknown until a shot shows it spending ammo or not.
    void CheckWeaponFireResult(float Now);

    UFortServerBotManagerAthena* Manager = nullptr;
    TWeakObjectPtr<AAIController> Controller;
    TWeakObjectPtr<AFortPlayerPawnAthena> PlayerBotPawn;
    int32_t BotIndex = 0;
    uint8_t Team = 0;
    std::wstring BotName;
    EFortAthenaAIBotState State = EFortAthenaAIBotState::Warmup;

    FAIBotSkills AIBotSkills;
    EAlertLevel CurrentAlertLevel = EAlertLevel::Unaware;
    TWeakObjectPtr<AFortPawn> CurrentTarget;
    bool bHasVisualLineOfSightOnTarget = false;
    FVector LastKnownPosition;

    std::vector<FFortAthenaAIBotWeapon> WeaponList;
    std::map<UFortItemDefinition*, int32_t> Ammo;
    int32_t EquippedIndex = -1;
    TWeakObjectPtr<AFortWeapon> EquippedWeapon;
    bool bPickaxeEquipped = false;
    int32_t Kills = 0;
    int32_t ShotsFired = 0;

    // Timers (world seconds).
    float JumpTime = 0.0f;
    float LatestJumpTime = 0.0f;
    float NextPerceptionTime = 0.0f;
    float NextDeathCheck = 0.0f;
    float NextFaceTime = 0.0f;
    float NextStormTick = 0.0f;
    float LastStormHealth = -1.0f;
    float NextLootSearch = 0.0f;
    float NextWanderTime = 0.0f;
    float NextShotTime = 0.0f;
    float FireUntil = 0.0f;
    bool bFiring = false;
    float ReloadUntil = 0.0f;
    float TargetAcquiredTime = 0.0f;
    float NextStuckCheck = 0.0f;
    float LandedTime = 0.0f;
    float NextLogTime = 0.0f;
    float WarmupRespawnTime = -1.0f;
    FVector LastStuckCheckLocation;
    int32_t StuckChecks = 0;

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
    bool IsUnreachable(const void* Object) const;
    void MarkUnreachable(const void* Object);
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
    bool bRegisteredInvoker = false;
    float NextObstacleProbe = 0.0f;
    float AvoidUntil = 0.0f;
    FVector AvoidDirection;
    int32_t Jumps = 0;
    float NextJumpTime = 0.0f;
    int32_t DoorsOpened = 0;
    bool bLoggedMovement = false;
    uint8_t MovementStyle = 0xFF;

    // The game's own fire (ActivateWeaponFire): the shot waiting to show whether it spent ammo.
    bool bWeaponFireActive = false;
    float WeaponFireCheckTime = 0.0f;
    int32_t WeaponFireAmmoBefore = 0;
    int32_t NativeShots = 0;
    FVector WanderGoal;
    bool bHasWanderGoal = false;
    FVector AimError;
};
