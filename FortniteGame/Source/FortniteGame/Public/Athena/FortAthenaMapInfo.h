#pragma once

#include "GameFramework/Actor.h"
#include "GameplayEffect.h"

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

    static bool HasTreasureChestSpawnPercent() { return Member_TreasureChestClass.IsAvailable() && Member_TreasureChestMinSpawnPercent.IsAvailable() && Member_TreasureChestMaxSpawnPercent.IsAvailable(); }
    static bool HasAmmoBoxSpawnPercent() { return Member_AmmoBoxClass.IsAvailable() && Member_AmmoBoxMinSpawnPercent.IsAvailable() && Member_AmmoBoxMaxSpawnPercent.IsAvailable(); }
};
