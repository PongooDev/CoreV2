#pragma once

#include "GameFramework/Actor.h"
#include "Items/FortItem.h"
#include "Math/Rotator.h"
#include "Math/Vector.h"
#include "Misc/Guid.h"
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
    DefineUProperty(FGuid, OptionalMissionGuid)
    DefineUProperty(int32_t, OptionalOwnerID)

    static AFortPickup* CreatePickup(UWorld* World, const FFortItemEntry& ItemEntry, const FVector* SpawnLocation = nullptr, const FRotator* SpawnRotation = nullptr, AFortPlayerController* OptionalOwner = nullptr, UClass* OverrideClass = nullptr);
    void TossPickup(const FVector& FinalLocation, AFortPawn* ItemOwner, int32_t OverrideMaxStackCount);
    void SetPickupTarget(AFortPawn* PickupTarget, float InFlyTime = -1.0f, const FVector& InStartDirection = FVector(0.0f, 0.0f, 1.0f), bool bPlayPickupSound = true);
    void GivePickupTo(IFortInventoryOwnerInterface* InventoryOwner);
    void SetOptionalMissionGuid(const FGuid& InMissionGuid);

    static ptrdiff_t PrimaryPickupItemEntryOffset();
    static void HookVTables();

private:
    static void GivePickupToHK(AFortPickup* This, IFortInventoryOwnerInterface* InventoryOwner);
};
