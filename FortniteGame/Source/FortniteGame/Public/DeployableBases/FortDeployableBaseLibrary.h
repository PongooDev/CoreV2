#pragma once

#include "Containers/Array.h"
#include "UObject/Object.h"
#include "UObject/Stack.h"

class AFortPlayerController;
class UFortWorldItemDefinition;

// Challenge the Horde's Blueprint helpers. 5.0 strips the server side of three of them (their execs call the shared
// empty function after reading the parameters): the combat zone's starting loot, the loot of each wave won, and the reset
// of the players' combat zone inventories. Core restores those three; the rest stay the build's.
class UFortDeployableBaseLibrary : public UObject
{
    DefineUnrealClass(UFortDeployableBaseLibrary, FortniteGame)
    DefineUFunction(SetupDeployableBaseCombatZoneInventoriesOnPlayers)
    DefineUFunction(GrantPlayersCompletedWaveRewards)
    DefineUFunction(ResetDeployableBaseHordePlayerInventories)
    DefineUFunction(IsWorldItemDefValidToPersistForDeployableBasePlayerInventory)

public:
    // Each player gets the loot of the tier's starting tier groups (the layout's loot set, StartOfCollectionItemTierGroups:
    // building, crafting and ammo) and of the Horde Skill Tree's CombatZoneStartLootGrantUnlocks they own.
    static void SetupDeployableBaseCombatZoneInventoriesOnPlayers(UObject* WorldContextObject, const TArray<FName>& TierGroups, const TArray<AFortPlayerController*>& PlayerControllers);
    // Every player gets the loot of the wave's tier groups (SuccessfulWaveItemTierGroups) and of the
    // CombatZoneWaveCompleteLootGrantUnlocks they own.
    static void GrantPlayersCompletedWaveRewards(UObject* WorldContextObject, int32_t Tier, int32_t Wave, int32_t NumberOfPlayers, const TArray<FName>& TierGroups);
    // What a combat zone gave (the items a deployable base keeps: resources, ammo, traps and ingredients) leaves every
    // player's inventory, before a tier and after it.
    static void ResetDeployableBaseHordePlayerInventories(UObject* WorldContextObject);
    static bool IsWorldItemDefValidToPersistForDeployableBasePlayerInventory(const UFortWorldItemDefinition* ItemDefinition);

    DECLARE_FUNCTION(execSetupDeployableBaseCombatZoneInventoriesOnPlayers);
    DECLARE_FUNCTION(execGrantPlayersCompletedWaveRewards);
    DECLARE_FUNCTION(execResetDeployableBaseHordePlayerInventories);

    static void HookNatives();
};
