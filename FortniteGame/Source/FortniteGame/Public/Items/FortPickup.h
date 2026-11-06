#pragma once

#include "GameFramework/Actor.h"
#include "Items/FortItem.h"
#include "Math/Rotator.h"
#include "Math/Vector.h"
#include "Misc/Guid.h"
#include "StructArray.h"

class AFortAIPawn;
class AFortInventory;
class AFortPawn;
class AFortPickup;
class AFortPlayerController;
class AFortPlayerPawn;
class IFortInventoryOwnerInterface;
class UProjectileMovementComponent;
class UWorld;

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

    // Whether the build reflects what pickup combining uses.
    static bool IsCombineAvailable();
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

    static float GetRandomFlyTime();
    static ptrdiff_t PrimaryPickupItemEntryOffset();
    static ptrdiff_t PickedUpOffset();
    static void HookVTables();

private:
    void GivePickupToPlayer(AFortPlayerController* PlayerController);
    void GivePickupToAI(AFortAIPawn* AIPawn, IFortInventoryOwnerInterface* InventoryOwner);
    // Same-item pickups that come to rest near each other merge into one (CombinePickups in FortPickup.cpp).
    void WatchForCombine();
    void StartCombine(AFortPickup* Target);
    void FinishCombine(AFortPickup* Target);
    static void CombinePickups();
    static void GivePickupToHK(AFortPickup* This, IFortInventoryOwnerInterface* InventoryOwner);
};
