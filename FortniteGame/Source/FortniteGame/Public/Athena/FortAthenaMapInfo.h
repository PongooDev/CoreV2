#pragma once

#include "GameFramework/Actor.h"
#include "GameplayEffect.h"
#include "Math/Box.h"

class AFortAthenaSupplyDrop;
class AVolume;
struct FRotator;
struct FVector;

// One storm circle of a match (AFortAthenaMapInfo::SafeZoneDefinitions, index: the game mode's SafeZonePhase): how long
// it holds before it shrinks, and how long it shrinks for (AthenaGameData's Default.SafeZone.NN.WaitTime and ShrinkTime
// rows, which a playlist's own game data replaces).
struct FFortSafeZoneDefinition
{
    DefineUnrealStruct(FFortSafeZoneDefinition, FortniteGame)
    DefineUStructProperty(FScalableFloat, Radius)
    DefineUStructProperty(FScalableFloat, WaitTime)
    DefineUStructProperty(FScalableFloat, ShrinkTime)
    static bool IsAvailable() { return StaticStruct() && Member_Radius.IsAvailable() && Member_WaitTime.IsAvailable() && Member_ShrinkTime.IsAvailable(); }
};

// Battle Royale's map settings (from 1.7.2; DefaultMapInfo_C, placed in Athena_Terrain). AFortGameModeAthena's own
// FinishWorldInitialization (1.7.2 0x3CF860) finds it and sets AFortGameStateAthena::MapInfo.
class AFortAthenaMapInfo : public AActor
{
    DefineUnrealClass(AFortAthenaMapInfo, FortniteGame)

public:
    // How many of the map's placed chests and ammo boxes a match keeps (AthenaGameData's Default.TreasureChest.* and
    // Default.AmmoBox.* rows). The 21.40 PDB still has them, as FFortTreasureChestSpawnInfo and FFortAmmoBoxSpawnInfo.
    DefineUProperty(UClass*, TreasureChestClass)
    DefineUStructProperty(FScalableFloat, TreasureChestMinSpawnPercent)
    DefineUStructProperty(FScalableFloat, TreasureChestMaxSpawnPercent)
    DefineUProperty(UClass*, AmmoBoxClass)
    DefineUStructProperty(FScalableFloat, AmmoBoxMinSpawnPercent)
    DefineUStructProperty(FScalableFloat, AmmoBoxMaxSpawnPercent)
    // The Supply Llama (from 3.3; AthenaSupplyDrop_Llama_C) and how many a match has (AthenaGameData's
    // Default.Llamas.QuantityMin and QuantityMax rows).
    DefineUProperty(UClass*, LlamaClass)
    DefineUStructProperty(FScalableFloat, LlamaQuantityMin)
    DefineUStructProperty(FScalableFloat, LlamaQuantityMax)
    // The vending machine (from 3.5; B_Athena_VendingMachine_C) and the weights of its rarities (AthenaGameData's
    // Default.VendingMachine.RarityWeights, levels 0 to 5: 75 for no machine, then 10 Common, 20 Uncommon, 20 Rare, 7.5
    // Epic and 5 Legendary).
    DefineUProperty(UClass*, VendingMachineClass)
    DefineUStructProperty(FScalableFloat, VendingMachineRarityCount)
    static bool HasVendingMachines() { return Member_VendingMachineClass.IsAvailable() && Member_VendingMachineRarityCount.IsAvailable(); }
    // The heights a supply drop's ground trace runs between (AthenaGameData's Default.SupplyDrop.MinPlacementHeight and
    // MaxPlacementHeight rows), which the "SPAWNLLAMA" command passes to SpawnSupplyDropAtLocation from 3.6 (Max as
    // TraceStartZ, Min as TraceEndZ).
    DefineUStructProperty(FScalableFloat, SupplyDropMinPlacementHeight)
    DefineUStructProperty(FScalableFloat, SupplyDropMaxPlacementHeight)
    // The match's storm circles, and how long after the safe zones start the first one comes (AthenaGameData's
    // Default.SafeZone.StartDelay).
    DefineUStructArrayProperty(FFortSafeZoneDefinition, SafeZoneDefinitions)
    DefineUStructProperty(FScalableFloat, SafeZoneStartDelay)
    static bool HasSafeZoneDefinitions() { return Member_SafeZoneDefinitions.IsAvailable() && FFortSafeZoneDefinition::IsAvailable(); }
    // Where the bus may drop players: the island, which PickSupplyDropLocation keeps its points in.
    DefineUProperty(FBox2D, AircraftDropZone)
    DefineUProperty(AVolume*, AircraftDropVolume)
    // How high the bus flies over the map info (AthenaGameData's Default.Aircraft.Height row, which a playlist's own game
    // data replaces from 1.11).
    DefineUStructProperty(FScalableFloat, AircraftHeight)
    static bool HasAircraftHeight() { return Member_AircraftHeight.IsAvailable(); }

    static bool HasTreasureChestSpawnPercent() { return Member_TreasureChestClass.IsAvailable() && Member_TreasureChestMinSpawnPercent.IsAvailable() && Member_TreasureChestMaxSpawnPercent.IsAvailable(); }
    static bool HasAmmoBoxSpawnPercent() { return Member_AmmoBoxClass.IsAvailable() && Member_AmmoBoxMinSpawnPercent.IsAvailable() && Member_AmmoBoxMaxSpawnPercent.IsAvailable(); }
    // Whether the build has Supply Llamas (3.3 on) and the native code that places them (Finder::ResolveSupplyDropSpawnLive).
    static bool HasLlamas();

    // The height the bus flies at in the world: AFortAthenaMapInfo::InitializeFlightPath (4.5 0xA3F020) starts every
    // aircraft path at the map info's own location's Z plus AircraftHeight at level 0. The game works it out inline; the
    // name is Core's. -1 where the build has no AircraftHeight.
    float GetAircraftFlightHeight();
    // A random point within Radius of Center (at its height) inside AircraftDropVolume, or Center when 1000 tries find none.
    FVector PickSupplyDropLocation(const FVector& Center, float Radius);
    // Spawns a SupplyDropClass on the ground under Location (AFortAthenaSupplyDrop::FindGroundLocationAt). From 3.6 its
    // ground trace runs from TraceStartZ down to TraceEndZ (the drop stays at TraceEndZ when it hits nothing); before,
    // FindGroundLocationAt read the heights from the map info and the two are ignored.
    AFortAthenaSupplyDrop* SpawnSupplyDropAtLocation(const FVector& Location, const FRotator& Rotation, UClass* SupplyDropClass, float TraceStartZ, float TraceEndZ);
    // Spawns the match's Supply Llamas at random points of the island (from 3.3; stripped, Core supplies its body).
    void SpawnLlamas();
    // Rolls a rarity for each of the map's placed vending machines from VendingMachineRarityCount: no machine (it is
    // destroyed), or Common to Legendary, which sets its StartingGoalLevel (prices 100 to 500 a material, its colour) and
    // the loot tier of the three items it sells (from 3.5; the server step is stripped, Core supplies it).
    void SpawnVendingMachines();
};
