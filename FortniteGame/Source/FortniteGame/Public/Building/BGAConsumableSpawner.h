#pragma once

#include "Building/BuildingActor.h"
#include "EnvironmentQuery/EnvQueryTypes.h"
#include "Items/FortItem.h"
#include "StructArray.h"

class UEnvQuery;
class UEnvQueryInstanceBlueprintWrapper;
class UWorld;

// A hidden point in the map that spawns building gameplay actors to pick up around it (4.0's BP_BGACSpawner_C: Hop
// Rocks in the craters, Loot_AthenaDurrrables by default). It rolls SpawnLootTierGroup, whose items are
// UBGAConsumableWrapperItemDefinitions, and places each roll's ConsumableClass on a point its QueryTemplate finds
// (EQS_ConsumableBGAPlacement_Circle_M: a donut of 200 to 750 around the spawner, traced onto the geometry, randomly
// scored). Its native code is all server side and stripped from the client: 4.0's vtable (0x3394348) overrides only the
// destructor of ABuildingActor's, so nothing ever spawns. 21.40's class has the same members plus the query's result,
// the consumable classes' streamable handle, bAthenaPostApplyPlaylistDataTables and bSpawnedAnything: the roll waits for
// the playlist's loot tables. Its functions' names are not in the 21.40 exports.
class ABGAConsumableSpawner : public ABuildingActor
{
    DefineUnrealClass(ABGAConsumableSpawner, FortniteGame)

public:
    DefineUProperty(FName, SpawnLootTierGroup)
    DefineUProperty(ABuildingActor*, AssociatedBuildingActor)
    DefineUProperty(UEnvQuery*, QueryTemplate)
    DefineUStructArrayProperty(FAIDynamicParam, QueryConfig)
    DefineUProperty(bool, bAlignSpawnedActorsToSurface)
    DefineUStructArrayProperty(FFortItemEntry, ConsumablesToSpawn)

    // Core's: each spawner of the world rolls and spawns its consumables, once the playlist's loot tables are applied
    // (called from AFortGameModeZone::FinishWorldInitialization). Called again when the match's playlist brings other
    // loot tables, it takes out what the earlier roll spawned and rolls again.
    static void SpawnWorldConsumables(UWorld* World);
    // Places the roll's consumables on the query's points (the query's finished delegate in Epic's).
    void OnQueryFinished(UEnvQueryInstanceBlueprintWrapper* QueryInstance);

private:
    void SpawnConsumables(int32_t WorldLevel);
};
