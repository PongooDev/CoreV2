#pragma once

#include "Building/BuildingSMActor.h"
#include "Math/Vector.h"
#include "UObject/AssetPtr.h"
#include "UObject/Stack.h"

class AAthenaCreativeRift;
class ABuildingRift;
class USceneComponent;

// Creative's Creature Spawner device (BP_DeimosSpawner, 7.40 on). 8.20 makes it an ABuildingProp; 7.40 and 8.00 have it as
// ABuildingTrapFloor_DeimosSpawner, an ABuildingTrapFloor. Both are ABuildingSMActors. At its minigame's start it spawns
// its creature rift (DeimosSpawnerClass, BP_DeimosRift_CubeCreative) and gives it the encounter settings its options pick;
// at the end (MinigameEnded, intact) it destroys the rift.
class ABuildingProp_DeimosSpawner : public ABuildingSMActor
{
    DefineRenamedUnrealClass(ABuildingProp_DeimosSpawner, ABuildingTrapFloor_DeimosSpawner, FortniteGame)
    DefineUFunction(MinigameStarted)
    DefineUFunction(SpawnRift)
    DefineUFunction(OnSpawnedRift)

public:
    DefineUProperty(UClass*, DeimosSpawnerClass)
    // The difficulty options the Blueprint's SetupConfigurables picks from the device's knobs as its minigame starts
    // (creature count, creature type, spawn timing; 8.20 adds the spawn limit).
    DefineUProperty(TAssetSubclassOf<UClass>, SelectedPawnNumberCap)
    DefineUProperty(TAssetSubclassOf<UClass>, SelectedSpawnGroup)
    DefineUProperty(TAssetSubclassOf<UClass>, SelectedSpawnTiming)
    DefineUProperty(TAssetSubclassOf<UClass>, SelectedSpawnLimit)
    // 8.20's other knobs, which SetupConfigurables copies here too.
    DefineOptionalUProperty(int32_t, ActivationRangeLevel)
    DefineOptionalUProperty(bool, bNoSpawnLimitEnabled)
    DefineOptionalUProperty(bool, bUseDistanceToAI)
    DefineOptionalUProperty(float, DespawnRangeOverride)
    DefineOptionalUProperty(bool, bDamageBuildingsOnSpawn)
    DefineOptionalUProperty(bool, bInvulnerable)
    DefineOptionalUProperty(bool, bVisible)
    DefineOptionalUProperty(float, OverrideQueryRadius)
    DefineUProperty(USceneComponent*, SpawnLocation)
    DefineUProperty(AAthenaCreativeRift*, CachedDeimosSpawner)

    void MinigameStarted_Implementation();
    void SpawnRift(const FVector& SpawnPosition);
    // The Blueprint event that sets the new rift up from the device's knobs (its damage on spawn and its VFX).
    void OnSpawnedRift(ABuildingRift* SpawnedRift);
    void HandleAsyncLoadComplete();

    DECLARE_FUNCTION(execSpawnRift);
    DECLARE_FUNCTION(execHandleAsyncLoadComplete);

    static void HookVTables();

private:
    // Core's: loads the selected options from Option on, one at a time; false when none is left to load.
    bool RequestSettingsLoad(int32_t Option);
    TAssetSubclassOf<UClass>* GetSelectedOption(int32_t Option) const;

    static void MinigameStartedHK(ABuildingProp_DeimosSpawner* This);
};
