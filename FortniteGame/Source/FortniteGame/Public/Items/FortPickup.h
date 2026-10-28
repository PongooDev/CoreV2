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

    // Merges entries of the same item and level up to its stack size, so a drop of five 1-plank rolls is one pickup of
    // five (CoreV2 #67). Without an inventory the stack size is the item's own.
    static void CombineItemEntries(TStructArray<FFortItemEntry>& Entries, AFortInventory* Inventory);
    static AFortPickup* CreatePickup(UWorld* World, const FFortItemEntry& ItemEntry, const FVector* SpawnLocation = nullptr, const FRotator* SpawnRotation = nullptr, AFortPlayerController* OptionalOwner = nullptr, UClass* OverrideClass = nullptr);
    // bToss false (1.8 on) puts the pickup at FinalLocation instead of tossing it there; before 1.8 a toss always flies.
    void TossPickup(const FVector& FinalLocation, AFortPawn* ItemOwner, int32_t OverrideMaxStackCount, bool bToss = true);
    // Tosses a container's loot from where it is onto FloorLocation, like TossOntoFloor, marked as tossed from it.
    void TossFromContainer(const FVector& FloorLocation);
    // Tosses the pickup from where it is in a short arc that lands it on FloorLocation, a point on the floor.
    void TossOntoFloor(const FVector& FloorLocation, AFortPawn* ItemOwner);
    void SetPickupTarget(AFortPawn* PickupTarget, float InFlyTime = -1.0f, const FVector& InStartDirection = FVector(0.0f, 0.0f, 1.0f), bool bPlayPickupSound = true);
    void GivePickupTo(IFortInventoryOwnerInterface* InventoryOwner);
    void SetOptionalMissionGuid(const FGuid& InMissionGuid);
    void OnRep_PickupLocationData();

    static float GetRandomFlyTime();
    static ptrdiff_t PrimaryPickupItemEntryOffset();
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
