#pragma once

#include "Containers/Array.h"
#include "GameFramework/Actor.h"
#include "GameplayTagContainer.h"
#include "StructArray.h"

class ADeployableBasePlot;
class AFortGameModeZone;
class UFortWorldItemDefinition;

// One plot of the manager's DeployableBaseArray (FDeployableBaseInstance: a fast array item and its plot).
struct FDeployableBaseArray
{
    DefineUnrealStruct(FDeployableBaseArray, FortniteGame)

public:
    DefineUProperty(TArray<void*>, DeployableBases)
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
    DefineUProperty(TArray<void*>, PendingUsers)
    DefineBitfieldUProperty(bWorldInitialized)

    static AFortDeployableBaseManager* GetCurrent(UObject* WorldContextObject);
    ADeployableBasePlot* GetDeployableBasePlotFromLocation(const FVector& Location) const;

    // Core's Challenge the Horde log (no game code is changed): the manager's state when the game mode finishes the
    // world's initialization, then the Horde mission's tiers and waves, its storm shields and the manager's plots and
    // pending players as they change, once a second until the zone ends.
    static void WatchHorde(AFortGameModeZone* GameMode);
};
