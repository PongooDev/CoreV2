#pragma once

#include "Containers/Array.h"
#include "Engine/NetSerialization.h"
#include "GameFramework/Actor.h"
#include "GameplayTagContainer.h"
#include "StructArray.h"

class ADeployableBasePlot;
class AFortGameModeZone;
class UFortWorldItemDefinition;

// One plot of the manager's DeployableBaseArray.
struct FDeployableBaseInstance : public FFastArraySerializerItem
{
    DefineUnrealStruct(FDeployableBaseInstance, FortniteGame)

public:
    DefineUProperty(ADeployableBasePlot*, DeployableBase)
};

struct FDeployableBaseArray : public FFastArraySerializer
{
    DefineUnrealStruct(FDeployableBaseArray, FortniteGame)

public:
    DefineUStructArrayProperty(FDeployableBaseInstance, DeployableBases)
};

// A player who joined before the plots were there: their fort's record (loaded from their cloud save, or a new one)
// and the plot they are given.
struct FPendingDeployableBaseUser
{
    DefineUnrealStruct(FPendingDeployableBaseUser, FortniteGame)

public:
    DefineUProperty(UObject*, BaseRecord)
    DefineUProperty(ADeployableBasePlot*, BasePlot)
};

// A loot grant the Horde Skill Tree unlocks (FFortTaggedDeployableBaseLootUnlock): its tier group, for a player who
// owns RequiredTag (FFortTaggedUnlockBase).
struct FFortTaggedDeployableBaseLootUnlock
{
    DefineUnrealStruct(FFortTaggedDeployableBaseLootUnlock, FortniteGame)

public:
    DefineUProperty(FGameplayTag, RequiredTag)
    DefineUProperty(FName, LootTierGroup)
};

// The zone theme's SupportedUnlocks (HordeCombatZoneUnlocks, NeighborhoodUnlocks).
class UFortDeployableBaseSkillTreeUnlocks : public UObject
{
    DefineUnrealClass(UFortDeployableBaseSkillTreeUnlocks, FortniteGame)

public:
    DefineUStructArrayProperty(FFortTaggedDeployableBaseLootUnlock, CombatZoneStartLootGrantUnlocks)
    DefineUStructArrayProperty(FFortTaggedDeployableBaseLootUnlock, CombatZoneWaveCompleteLootGrantUnlocks)
};

// The deployable bases (Challenge the Horde's and Build the Fort's plots): AFortGameStateDeployableBase's
// InitializeDeployableBaseManager spawns it from the zone theme's DeployableBaseSettings.
class AFortDeployableBaseManager : public AActor
{
    DefineUnrealClass(AFortDeployableBaseManager, FortniteGame)
    DefineUFunction(GetCurrent)
    DefineUFunction(GetDeployableBasePlotFromLocation)

public:
    DefineUStructProperty(FDeployableBaseArray, DeployableBaseArray)
    DefineUProperty(UObject*, DeployableBaseItemDefinition)
    DefineUProperty(UClass*, DeployableBasePlotClass)
    DefineUProperty(UFortDeployableBaseSkillTreeUnlocks*, SupportedSkillTreeBasedUnlocks)
    DefineUProperty(int32_t, NumExpectedPlots)
    DefineUStructArrayProperty(FPendingDeployableBaseUser, PendingUsers)
    DefineBitfieldUProperty(bWorldInitialized)

    static AFortDeployableBaseManager* GetCurrent(UObject* WorldContextObject);
    ADeployableBasePlot* GetDeployableBasePlotFromLocation(const FVector& Location) const;

    // The manager's part of the world's initialization, which AFortGameModeDeployableBase::FinishWorldInitialization
    // hands it. 5.0 strips its plot spawn (a call to the shared return-zero stub with the world), so the plot check that
    // follows always fails and restarts the dedicated server (CoreV2 #353). Core runs it with the plots restored: true
    // when it did, false to leave it to the build (not a deployable base zone, or the build's code isn't as expected).
    // GameModeFinish is the deployable base game mode's own FinishWorldInitialization.
    static bool FinishWorldInitialization(AFortGameModeZone* GameMode, void* GameModeFinish);
    // Finds the manager's world initialization and the natives it calls from AFortGameModeDeployableBase's
    // FinishWorldInitialization, at boot, so the log shows them before any zone.
    static bool ResolveWorldInitialization(void* GameModeFinish);

    // Core's Challenge the Horde log (no game code is changed): the manager's state when the game mode finishes the
    // world's initialization, then the Horde mission's tiers and waves, its storm shields and the manager's plots and
    // pending players as they change, once a second until the zone ends.
    static void WatchHorde(AFortGameModeZone* GameMode);

private:
    // The stripped piece: a plot of DeployableBasePlotClass at each of the world's PlacementActor.DeployableBasePlot
    // placement actors (HordeProto_003's four), in DeployableBaseArray.
    int32_t SpawnDeployableBasePlots(UWorld* World);
    void OnWorldInitialized();
};
