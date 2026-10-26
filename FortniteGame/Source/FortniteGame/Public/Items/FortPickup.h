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
class AFortPlayerController;
class IFortInventoryOwnerInterface;
class UWorld;

class AFortPickup : public AActor
{
    DefineUnrealClass(AFortPickup, FortniteGame)
    DefineUFunction(TossPickup)

public:
    DefineUStructProperty(FFortItemEntry, PrimaryPickupItemEntry)
    DefineUStructArrayProperty(FFortItemEntry, MultiItemPickupEntries)
    DefineUProperty(FGuid, OptionalMissionGuid)
    DefineUProperty(int32_t, OptionalOwnerID)
    DefineUProperty(AFortPawn*, PawnWhoDroppedPickup)

    // Merges entries of the same item and level up to its stack size, so a drop of five 1-plank rolls is one pickup of
    // five (CoreV2 #67).
    static void CombineItemEntries(TStructArray<FFortItemEntry>& Entries, AFortInventory* Inventory);
    static AFortPickup* CreatePickup(UWorld* World, const FFortItemEntry& ItemEntry, const FVector* SpawnLocation = nullptr, const FRotator* SpawnRotation = nullptr, AFortPlayerController* OptionalOwner = nullptr, UClass* OverrideClass = nullptr);
    void TossPickup(const FVector& FinalLocation, AFortPawn* ItemOwner, int32_t OverrideMaxStackCount);
    void SetPickupTarget(AFortPawn* PickupTarget, float InFlyTime = -1.0f, const FVector& InStartDirection = FVector(0.0f, 0.0f, 1.0f), bool bPlayPickupSound = true);
    void GivePickupTo(IFortInventoryOwnerInterface* InventoryOwner);
    void SetOptionalMissionGuid(const FGuid& InMissionGuid);

    static float GetRandomFlyTime();
    static ptrdiff_t PrimaryPickupItemEntryOffset();
    static void HookVTables();

private:
    void GivePickupToAI(AFortAIPawn* AIPawn, IFortInventoryOwnerInterface* InventoryOwner);
    static void GivePickupToHK(AFortPickup* This, IFortInventoryOwnerInterface* InventoryOwner);
};
