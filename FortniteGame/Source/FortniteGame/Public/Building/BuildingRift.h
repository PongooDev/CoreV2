#pragma once

#include "AI/FortAIEncounterInfo.h"
#include "Building/BuildingActor.h"
#include "GameplayEffect.h"

class ABuildingProp_DeimosSpawner;
class USphereComponent;

// A rift the AI comes out of: a mission's (AFortMission_RiftSpawners) spawner while it is registered there.
class ABuildingRift : public ABuildingActor
{
    DefineUnrealClass(ABuildingRift, FortniteGame)
    DefineUFunction(BlueprintHideRift)

public:
    // How far around the rift its spawn slots are searched for (EnvironmentQuery's radius).
    DefineUProperty(float, QueryRadius)
    // The overlap sphere a pawn starts the rift's encounter in, and its radius by level: BeginPlay sizes it at the game's
    // difficulty level.
    DefineUStructProperty(FScalableFloat, OverlapSphereRadius)
    DefineUProperty(USphereComponent*, OverlapSphereComponent)

    // The Blueprint event that hides the rift (BlueprintEvent; Creative's creature rift multicasts HideSpawner).
    void BlueprintHideRift();
};

// The rift a Creative creature spawner (ABuildingProp_DeimosSpawner) spawns at the start of its game (7.40 adds it).
class AAthenaCreativeRift : public ABuildingRift
{
    DefineUnrealClass(AAthenaCreativeRift, FortniteGame)

public:
    // 8.20: the players in it keep its encounter going; the last to leave sends "CreativeDespawnEndOverlap".
    DefineOptionalUProperty(USphereComponent*, DespawnSphereComponent)
    DefineUProperty(ABuildingProp_DeimosSpawner*, ParentTrap)
    // 8.00+ (7.40 doesn't reflect them).
    DefineUStructProperty(FFortEncounterSettings, OverrideEncounterSettings)
    DefineUProperty(bool, bHasLoadedSettings)

    static bool HasEncounterSettings() { return Member_OverrideEncounterSettings.IsAvailable() && Member_bHasLoadedSettings.IsAvailable() && Member_ParentTrap.IsAvailable(); }

    void BeginPlay();

    static void HookVTables();

private:
    static inline void (*BeginPlayOG)(AAthenaCreativeRift* This) = nullptr;
    static void BeginPlayHK(AAthenaCreativeRift* This);
};
