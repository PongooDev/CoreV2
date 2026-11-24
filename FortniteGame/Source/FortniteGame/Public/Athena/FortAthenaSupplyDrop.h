#pragma once

#include "Building/BuildingActor.h"
#include "UObject/Stack.h"

class AFortPawn;
class AFortPickup;
class UFortWorldItemDefinition;
struct FFortItemEntry;
struct FVector;

// Battle Royale's supply drop (AthenaSupplyDrop_02_C; an ABuildingGameplayActor). From 1.10 its SpawnTierDrops spawns
// each item PickLootDrops rolls through SpawnPickup, where 1.9.1 and earlier called UFortKismetLibrary::K2_SpawnPickupInWorld.
class AFortAthenaSupplyDrop : public ABuildingActor
{
    DefineUnrealClass(AFortAthenaSupplyDrop, FortniteGame)
    DefineUFunction(SpawnPickup)
    DefineUFunction(SpawnPickupFromItemEntry)
    DefineUFunction(SpawnGameModePickup)

public:
    AFortPickup* SpawnPickup(UFortWorldItemDefinition* ItemDefinition, int32_t NumberToSpawn, AFortPawn* TriggeringPawn, const FVector& Position, const FVector& Direction);
    // 7.x: the pickup of a rolled item entry (its level and state kept), NumberToSpawn of it. Save the World's Frostnite
    // supply drops (EndlessModeSupplyDrop_C's SpawnLoot) spawn each item PickLootDropsWithNamedWeights rolls with it.
    AFortPickup* SpawnPickupFromItemEntry(UFortWorldItemDefinition* ItemDefinition, const FFortItemEntry& InItemEntry, int32_t NumberToSpawn, AFortPawn* TriggeringPawn, const FVector& Position, const FVector& Direction);

    // 8.50: a pickup of PickupClass (an AFortGameModePickup; that class itself when none or another is given) holding
    // NumberToSpawn of ItemDefinition. Endgame's stone supply drops (AthenaSupplyDrop_Rock_*) land their Infinity Stone
    // with it.
    AFortPickup* SpawnGameModePickup(UFortWorldItemDefinition* ItemDefinition, UClass* PickupClass, int32_t NumberToSpawn, AFortPawn* TriggeringPawn, const FVector& Position, const FVector& Direction);

    DECLARE_FUNCTION(execSpawnPickup);
    DECLARE_FUNCTION(execSpawnGameModePickup);
    DECLARE_FUNCTION(execSpawnPickupFromItemEntry);

    static void HookNatives();

private:
    static void HookSpawnPickup();
    static void HookSpawnPickupFromItemEntry();
    static void HookSpawnGameModePickup();
};
