#pragma once

#include "Building/BuildingActor.h"
#include "Engine/DataTable.h"
#include "GameplayEffect.h"
#include "Items/FortItem.h"
#include "Math/Rotator.h"
#include "Math/Vector.h"
#include "StructArray.h"

#include <cstdint>

class AFortPlayerController;
class UFortWorldItemDefinition;

// One of a collector's inputs: the item it takes, how many (InputCount at the collector's StartingGoalLevel, 3.5.1
// 0xA8D970), and what it gives for them. Epic spelled it FColletorUnitInfo up to 6.02 and corrected it in 6.10
// (CL-4480234).
struct FCollectorUnitInfo
{
    DefineRenamedUnrealStruct(FCollectorUnitInfo, FColletorUnitInfo, FortniteGame)

public:
    DefineUProperty(UFortWorldItemDefinition*, InputItem)
    DefineUStructProperty(FScalableFloat, InputCount)
    DefineBitfieldUProperty(bUseDefinedOutputItem)
    DefineUProperty(UFortWorldItemDefinition*, OutputItem)
    // Transient, replicated with ItemCollections: the client's screen shows the first entry's icon (0xAAB680).
    DefineUStructArrayProperty(FFortItemEntry, OutputItemEntry)
    DefineUProperty(FName, OverrideOutputItemLootTierGroupName)
};

// FPrimaryAssetId's layout: the asset's primary type (FPrimaryAssetType, its one FName) and its name.
struct FItemCollectorPrimaryAssetId
{
    FName PrimaryAssetType;
    FName PrimaryAssetName;
};

// A row of a collector's OverrideItemsTable (6.20's Playground: ItemCollectorItems_ChallengeGames_01 to _05,
// _SpikeArena_01 to _03...): an item, by its primary asset id, and how many.
struct FItemCollectorOverrideItemRow
{
    DefineUnrealStruct(FItemCollectorOverrideItemRow, FortniteGame)

public:
    DefineUProperty(FItemCollectorPrimaryAssetId, ItemPrimaryAssetId)
    DefineUProperty(int32_t, Quantity)
};

// A row of the table a collector's ItemCollectorHandle names (6.20's Playground ItemCollectorData): with
// bOverrideLootRules its inputs sell OverrideItemsTable's items instead of a loot roll, with bUseOverrideRarity it has
// OverrideRarity (EFortRarity: Handmade, Common, to Fine, Legendary), and OverrideGoal is its price (GetDepositGoal).
struct FItemCollectorRow
{
    DefineUnrealStruct(FItemCollectorRow, FortniteGame)

public:
    DefineUProperty(UDataTable*, OverrideItemsTable)
    DefineUProperty(uint8_t, OverrideRarity)
    DefineUProperty(int32_t, bOverrideLootRules)
    DefineUProperty(int32_t, bUseOverrideRarity)
};

// A building that takes items for others (from 3.0; ABuildingGameplayActor): Save the World's BluGlo pylons, and from 3.5
// Battle Royale's vending machines (B_Athena_VendingMachine_C: wood, stone and metal, FreeForAll, Loot_AthenaVending).
// Its own code is in the client: BeginPlay picks an input and starts cycling them (3.5.1 0xA84B40, every
// ActiveItemRotationTime), a melee hit cycles at once (0xA98C10), and its IFortInteractInterface::ServerOnAttemptInteract
// (0xAA1F00) takes the input (UFortKismetLibrary::K2_RemoveItemFromPlayer) and checks the goal (CheckDepositGoalReached,
// 0xA866C0), which then waits DespensingDelayTime to give the output. That last step, GrantOutput, is stripped.
class ABuildingItemCollectorActor : public ABuildingActor
{
    DefineUnrealClass(ABuildingItemCollectorActor, FortniteGame)

public:
    DefineUProperty(uint8_t, ItemCollectorBehavior)
    DefineUProperty(FName, DefaultItemLootTierGroupName)
    DefineUStructArrayProperty(FCollectorUnitInfo, ItemCollections)
    DefineUProperty(UFortWorldItemDefinition*, ActiveInputItem)
    DefineUProperty(FVector, LootSpawnLocation)
    DefineUProperty(FRotator, LootTossDirection)
    DefineUProperty(float, LootTossConeHalfAngle)
    DefineUProperty(float, LootTossSpeed)
    DefineUProperty(bool, bTossOnGround)
    // Replicated: the inputs' InputCount level, and a vending machine's rarity (its screen and lights: StartingSeedValue
    // of UpdateCollectorsActiveItem, 0 Common to 4 Legendary). BeginPlay sets it to 0 (0xA84BEA); nothing else does.
    DefineUProperty(int32_t, StartingGoalLevel)
    // From 6.20: the collector's row of an ItemCollectorRow table (Playground's machines: ItemCollectorData).
    DefineUStructProperty(FDataTableRowHandle, ItemCollectorHandle)

    static bool HasOutputItems() { return Member_ItemCollections.IsAvailable() && Member_StartingGoalLevel.IsAvailable() && FCollectorUnitInfo::StaticStruct(); }
    // The collector's ItemCollectorHandle row, or null (no handle on this build, or none set).
    const FItemCollectorRow* GetItemCollectorRow() const;

    // 21.40's name, server only: rolls each input's OutputItemEntry from the rarity's loot tier group where the tables
    // have one, else its own (the input's OverrideOutputItemLootTierGroupName, else DefaultItemLootTierGroupName) at the
    // StartingGoalLevel loot tier, so the three inputs of a vending machine sell three items of its rarity. From 6.20 a
    // collector whose ItemCollectorHandle row overrides the loot sells that row's items instead.
    void FinishItemSelectionSetup();
    // 21.40's name, server only: what the collector gives a player who reached the goal of InputItem's input, as
    // pickups out of its front (LootSpawnLocation, LootTossDirection).
    void GrantOutput(AFortPlayerController* PlayerController, UFortWorldItemDefinition* InputItem);

    // The timer delegate CheckDepositGoalReached sets (its lambda: the collector, the controller and the input, all
    // weak) is left with no GrantOutput call; its instance's Execute and ExecuteIfSafe slots become Core's. And from
    // 6.20, BeginPlay's call of FinishItemSelectionSetup (the client half the builds keep) goes through
    // FinishItemSelectionSetupHK, for the collectors that begin play after the match's machines were set up.
    static void HookVTables();

private:
    // A collector beginning play once the playlist's tables are applied (a Playground playset's, streamed with its level):
    // the build's FinishItemSelectionSetup, then the server half (AFortAthenaMapInfo::SetUpLateVendingMachine).
    static void FinishItemSelectionSetupHK(ABuildingItemCollectorActor* This);
    // Its OverrideItemsTable's items for the inputs, in row order. False when the row doesn't override the loot.
    bool ApplyOverrideItems(const FItemCollectorRow& Row);
    static void GrantOutputDelegateExecuteHK(void* DelegateInstance);
    static bool GrantOutputDelegateExecuteIfSafeHK(void* DelegateInstance);
};
