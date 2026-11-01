#pragma once

#include "Building/BuildingActor.h"
#include "UObject/Stack.h"

class AFortPawn;
class AFortPickup;
class UFortWorldItemDefinition;
struct FVector;

// Battle Royale's supply drop (AthenaSupplyDrop_02_C; an ABuildingGameplayActor). From 1.10 its SpawnTierDrops spawns
// each item PickLootDrops rolls through SpawnPickup, where 1.9.1 and earlier called UFortKismetLibrary::K2_SpawnPickupInWorld.
class AFortAthenaSupplyDrop : public ABuildingActor
{
    DefineUnrealClass(AFortAthenaSupplyDrop, FortniteGame)
    DefineUFunction(SpawnPickup)

public:
    AFortPickup* SpawnPickup(UFortWorldItemDefinition* ItemDefinition, int32_t NumberToSpawn, AFortPawn* TriggeringPawn, const FVector& Position, const FVector& Direction);

    DECLARE_FUNCTION(execSpawnPickup);

    static void HookNatives();
};
