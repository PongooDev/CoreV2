#pragma once

#include "GameFramework/Actor.h"
#include "Items/FortItem.h"
#include "Math/Rotator.h"
#include "Math/Vector.h"
#include "StructArray.h"

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

    static AFortPickup* CreatePickup(UWorld* World, const FFortItemEntry& ItemEntry, const FVector* SpawnLocation = nullptr, const FRotator* SpawnRotation = nullptr, AFortPlayerController* OptionalOwner = nullptr, UClass* OverrideClass = nullptr);
    void TossPickup(const FVector& FinalLocation, AFortPawn* ItemOwner, int32_t OverrideMaxStackCount);
    void SetPickupTarget(AFortPawn* PickupTarget, float InFlyTime = -1.0f, const FVector& InStartDirection = FVector(0.0f, 0.0f, 1.0f), bool bPlayPickupSound = true);
    void GivePickupTo(IFortInventoryOwnerInterface* InventoryOwner);

    static void HookVTables();

private:
    static void GivePickupToHK(AFortPickup* This, IFortInventoryOwnerInterface* InventoryOwner);
};
