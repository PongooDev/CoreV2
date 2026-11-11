#pragma once

#include "GameFramework/Actor.h"
#include "Items/FortItem.h"
#include "Math/Rotator.h"
#include "Math/Vector.h"
#include "Misc/Guid.h"
#include "StructArray.h"
#include "UObject/ScriptDelegates.h"
#include "UObject/Stack.h"

class AFortAIPawn;
class AFortInventory;
class AFortPawn;
class AFortPickup;
class AFortPlayerController;
class AFortPlayerPawn;
class IFortInventoryOwnerInterface;
class UFunction;
class UProjectileMovementComponent;
class UWorld;
struct FHitResult;

class EFortPickupTossState
{
    DefineUnrealEnum(EFortPickupTossState, FortniteGame)
    DefineUEnumValue(NotTossed)
    DefineUEnumValue(InProgress)
    DefineUEnumValue(AtRest)
};

struct FFortPickupLocationData
{
    DefineUnrealStruct(FFortPickupLocationData, FortniteGame)

public:
    DefineUProperty(AFortPickup*, CombineTarget)
    DefineUProperty(FVector, LootInitialPosition)
    DefineUProperty(float, FlyTime)
    DefineUProperty(EFortPickupTossState, TossState)
    // Where a pickup at rest lies: the toss branch of OnRep_PickupLocationData (AFortPickup::SetupForMovementCompToss,
    // 4.5 0xDC75B0, 5.0 CL-4204761 0xF533F0) puts an AtRest pickup there.
    DefineUProperty(FVector, FinalTossRestLocation)

    // Whether the build reflects what pickup combining uses.
    static bool IsCombineAvailable();
    // Whether the build reflects FinalTossRestLocation and EFortPickupTossState's AtRest.
    static bool CanRestAt();
};

// 4.1 (CL-4053532) on: what AFortPickup::CreateFromData (21.40's STAT_CreatePickupFromData; 4.1 0xD73420) takes in
// place of CreatePickup's arguments. Native only, so the layout is 4.1's, read from CreateFromData and its callers
// (SpawnMissionItemPickup 0xDB6190, SetPickupTarget 0xD8D3A0, SpawnDropPickup 0xF97E50); the PDB has no type for it,
// so the names of the last members are Core's, after what CreateFromData does with them.
struct FFortCreatePickupData
{
    UWorld* World = nullptr;
    const FFortItemEntry* ItemEntry = nullptr;
    const FVector* SpawnLocation = nullptr;
    const FRotator* SpawnRotation = nullptr;
    // The pickup's owner when bOnlyRelevantToOwner is set; given to the pickup (0xD8D1F0) either way.
    AFortPlayerController* PlayerController = nullptr;
    UClass* OverrideClass = nullptr;
    // The spawn's owner otherwise; a pickup spawned at it is raised above its root component.
    AActor* SpawnOwner = nullptr;
    // AFortPickup::bRandomRotation.
    bool bRandomRotation = false;
    // Spawns the pickup bOnlyRelevantToOwner, owned by PlayerController.
    bool bOnlyRelevantToOwner = false;
    // EFortPickupSourceTypeFlag bits, or'd into AFortPickup::PickupSourceTypeFlags; Epic's callers pass Other (15).
    uint32_t PickupSourceTypeFlags = 15;
};
static_assert(sizeof(FFortCreatePickupData) == 0x40);

class AFortPickup : public AActor
{
    DefineUnrealClass(AFortPickup, FortniteGame)
    DefineUFunction(TossPickup)
    DefineUFunction(OnRep_PickupLocationData)
    // From 4.0: what the movement component's OnProjectileStop calls on the server (PostInitializeComponents binds it).
    DefineUFunction(OnServerStopCallback)

public:
    DefineUStructProperty(FFortItemEntry, PrimaryPickupItemEntry)
    DefineUStructArrayProperty(FFortItemEntry, MultiItemPickupEntries)
    DefineUStructProperty(FFortPickupLocationData, PickupLocationData)
    DefineUProperty(FGuid, OptionalMissionGuid)
    DefineUProperty(int32_t, OptionalOwnerID)
    DefineUProperty(AFortPawn*, PawnWhoDroppedPickup)
    DefineUProperty(UProjectileMovementComponent*, MovementComponent)
    DefineBitfieldUProperty(bPickedUp)
    // From 1.7.2 (Battle Royale): replicated so clients play the toss from a container.
    DefineUProperty(bool, bTossedFromContainer)
    // From 3.0: replicated, so clients show the pickup at a random rotation (K2_SpawnPickupInWorld's bRandomRotation).
    DefineUProperty(bool, bRandomRotation)
    static bool HasRandomRotation() { return Member_bRandomRotation.IsAvailable(); }
    // From 4.0: whether the pickup merges into one of the same stack near it when its toss completes (its movement's
    // OnProjectileStop, OnServerStopCallback). Server code alone reads it; 21.40 sets it with SetCombinePickupsWhenTossCompletes.
    DefineUProperty(bool, bCombinePickupsWhenTossCompletes)
    // From 4.5: bumped on the server when the pickup lands with its landed sound, so clients that don't simulate its
    // movement play the sound too (OnRep_ServerImpactSoundFlash, 4.5 0xDC2B50, 5.40 0xFA20A0: the movement's
    // bSimulationEnabled off, as a pickup with bClientUseInterpolationOnly has it on clients, 5.40 0xFA817E, and a
    // second since LastLandedSoundPlayTime).
    DefineUProperty(uint8_t, ServerImpactSoundFlash)
    // From 4.5: when the pickup last played its landed sound. The pickup's NotifyHit (4.5 0xDC1410, 5.40 0xFA0A60) plays
    // it where the pickup moves itself into a hit, a second or more after the last (5.40: also only for a hit whose Z
    // force, the velocity's Z times the normal's, passes LandSoundZForceThreshold), and sets this to the world's time.
    DefineUProperty(float, LastLandedSoundPlayTime)
    // What SetPickupTarget broadcasts as a pawn starts picking the pickup up (1.8.1 0x6B696B, 3.2 0xC9D773, 5.40
    // 0xFA6D46), before it sets bPickedUp: the pickup, the pawn, the item and where it goes (FPickupOnPickup, 36 bytes
    // on 2.4.2, 3.3 and 5.40).
    DefineUProperty(FMulticastScriptDelegate, OnPickupAttempted)

    // Merges entries of the same item and level up to its stack size, so a drop of five 1-plank rolls is one pickup of
    // five (CoreV2 #67). Without an inventory the stack size is the item's own.
    static void CombineItemEntries(TStructArray<FFortItemEntry>& Entries, AFortInventory* Inventory);
    static AFortPickup* CreatePickup(UWorld* World, const FFortItemEntry& ItemEntry, const FVector* SpawnLocation = nullptr, const FRotator* SpawnRotation = nullptr, AFortPlayerController* OptionalOwner = nullptr, UClass* OverrideClass = nullptr);
    // bToss false (1.8 on) puts the pickup at FinalLocation instead of tossing it there; before 1.8 a toss always flies.
    void TossPickup(const FVector& FinalLocation, AFortPawn* ItemOwner, int32_t OverrideMaxStackCount, bool bToss = true);
    // Tosses a container's loot from where it is at LaunchVelocity (the container's toss), headed for FinalLocation, marked
    // as tossed from it.
    void TossFromContainer(const FVector& FinalLocation, const FVector& LaunchVelocity);
    // Tosses the pickup from where it is in a short arc that lands it on FloorLocation, a point on the floor. It rises 70
    // over its start (or the landing, if higher), or as far as LaunchUpSpeed takes it when one is given.
    void TossOntoFloor(const FVector& FloorLocation, AFortPawn* ItemOwner, float LaunchUpSpeed = 0.0f);
    void SetPickupTarget(AFortPawn* PickupTarget, float InFlyTime = -1.0f, const FVector& InStartDirection = FVector(0.0f, 0.0f, 1.0f), bool bPlayPickupSound = true);
    // The pickup's own IFortInteractInterface::CanInteract for a simple interaction (a press, or walking over it) by
    // RequestingPawn: false once it is picked up or flying into another pickup, and whatever its class adds.
    bool CanInteract(AFortPlayerPawn* RequestingPawn);
    void GivePickupTo(IFortInventoryOwnerInterface* InventoryOwner);
    void SetOptionalMissionGuid(const FGuid& InMissionGuid);
    void OnRep_PickupLocationData();

    // 21.40's: whether the pickup merges into one of the same stack near it when its toss completes. Core's tosses set
    // it (TossPickup). Before 4.0 the build has no such property and Core's binding of OnServerStopCallback stands for it.
    void SetCombinePickupsWhenTossCompletes(bool bInCombinePickupsWhenTossCompletes);
    // The server's end of a toss: the movement component's OnProjectileStop (StopSimulating) calls it. The build's own
    // part (4.0 on, 4.1 0xD87880 to 5.40 0xFA2520) sets bServerStoppedSimulation, so clients stop simulating the toss;
    // the server code the client build leaves out merges the pickup into one near it (CombinePickups).
    void OnServerStopCallback(const FHitResult& Hit);
    // 21.40's server bounce callback (OnProjectileBounce): from 4.5, a bounce the pickup's NotifyHit played the landed
    // sound for bumps ServerImpactSoundFlash.
    void OnServerBounceCallback(const FHitResult& Hit, const FVector& ImpactVelocity);

    static float GetRandomFlyTime();
    static ptrdiff_t PrimaryPickupItemEntryOffset();
    static ptrdiff_t PickedUpOffset();
    static void HookVTables();
    // Core's OnServerStopCallback in the build's UFunction (4.0 on) or as a native of its own (before 4.0), its
    // OnServerBounceCallback (4.5 on) and HandleCombineTargetPickupAttempted. At boot, after
    // Finder::ResolveClassFunctionMap.
    static void HookNatives();

private:
    void GivePickupToPlayer(AFortPlayerController* PlayerController);
    void GivePickupToAI(AFortAIPawn* AIPawn, IFortInventoryOwnerInterface* InventoryOwner);
    // Binds what the server's toss calls on Core's tosses: OnServerStopCallback before 4.0 (4.0 on, the build's
    // PostInitializeComponents binds it) and OnServerBounceCallback.
    void BindServerMovementCallbacks();
    // Merges the pickup, its toss complete, into the nearest pickup of the same stack with room for it
    // (FindExistingPickupNear): it flies in over CombineFlyTime (StartCombine), then the target takes what fits.
    void CombinePickups(const AFortPickup* Except = nullptr);
    // 21.40's name: the resting pickup of the same stack within reach with room for this one, or null. Except is left
    // out (a pickup that is being picked up).
    AFortPickup* FindExistingPickupNear(const AFortPickup* Except);
    void StartCombine(AFortPickup* Target);
    // Takes the pickup's combine off the combines in flight: Serial's (the timer's own call) or, with 0, whichever it has,
    // clearing its timer. It no longer hears its target's OnPickupAttempted. Returns the target (null when it is gone)
    // and in bOutFound whether there was one.
    AFortPickup* TakeCombine(bool& bOutFound, uint64_t Serial);
    // CombineFlyTime is up (the timer StartCombine sets, Serial): the target takes what fits, or the pickup goes back.
    void EndCombine(uint64_t Serial);
    void FinishCombine(AFortPickup* Target);
    // Ends a combine that didn't take the whole pickup: it goes back to where it lay when the combine started, at rest,
    // and tries again (CombinePickups), leaving Except out.
    void CancelCombine(const AFortPickup* Except = nullptr);
    // The combine target's OnPickupAttempted: a pawn picks the target up, so this pickup goes back at once.
    void HandleCombineTargetPickupAttempted(AFortPickup* AttemptedPickup);
    // Where Core can't hear a toss end (no OnServerStopCallback of the build's or its own), the tossed pickups it watches.
    static void WatchTossedPickups();

    DECLARE_FUNCTION(execOnServerStopCallback);
    DECLARE_FUNCTION(execOnServerBounceCallback);
    DECLARE_FUNCTION(execHandleCombineTargetPickupAttempted);

    // The UFunction OnProjectileStop calls (the build's from 4.0, Core's before), and the build's OnServerStopCallback slot.
    static inline UFunction* OnServerStopCallbackFunction = nullptr;
    static inline int32_t OnServerStopCallbackIndex = -1;
    // Before 4.0: Core binds its OnServerStopCallback to its tosses' OnProjectileStop.
    static inline bool bBindServerStopCallback = false;
    static inline UFunction* OnServerBounceCallbackFunction = nullptr;
    static inline UFunction* HandleCombineTargetPickupAttemptedFunction = nullptr;

    static void GivePickupToHK(AFortPickup* This, IFortInventoryOwnerInterface* InventoryOwner);
};
