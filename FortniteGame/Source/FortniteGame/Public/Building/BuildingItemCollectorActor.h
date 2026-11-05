#pragma once

#include "Building/BuildingActor.h"
#include "GameplayEffect.h"
#include "Items/FortItem.h"
#include "Math/Rotator.h"
#include "Math/Vector.h"
#include "StructArray.h"

#include <cstdint>

class AFortPlayerController;
class UFortWorldItemDefinition;

// One of a collector's inputs (Epic's spelling): the item it takes, how many (InputCount at the collector's
// StartingGoalLevel, 3.5.1 0xA8D970), and what it gives for them.
struct FColletorUnitInfo
{
    DefineUnrealStruct(FColletorUnitInfo, FortniteGame)

public:
    DefineUProperty(UFortWorldItemDefinition*, InputItem)
    DefineUStructProperty(FScalableFloat, InputCount)
    DefineBitfieldUProperty(bUseDefinedOutputItem)
    DefineUProperty(UFortWorldItemDefinition*, OutputItem)
    // Transient, replicated with ItemCollections: the client's screen shows the first entry's icon (0xAAB680).
    DefineUStructArrayProperty(FFortItemEntry, OutputItemEntry)
    DefineUProperty(FName, OverrideOutputItemLootTierGroupName)
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
    DefineUProperty(FName, DefaultItemLootTierGroupName)
    DefineUStructArrayProperty(FColletorUnitInfo, ItemCollections)
    DefineUProperty(UFortWorldItemDefinition*, ActiveInputItem)
    DefineUProperty(FVector, LootSpawnLocation)
    DefineUProperty(FRotator, LootTossDirection)
    DefineUProperty(float, LootTossConeHalfAngle)
    DefineUProperty(float, LootTossSpeed)
    DefineUProperty(bool, bTossOnGround)
    // Replicated: the inputs' InputCount level, and a vending machine's rarity (its screen and lights: StartingSeedValue
    // of UpdateCollectorsActiveItem, 0 Common to 4 Legendary). BeginPlay sets it to 0 (0xA84BEA); nothing else does.
    DefineUProperty(int32_t, StartingGoalLevel)

    static bool HasOutputItems() { return Member_ItemCollections.IsAvailable() && Member_StartingGoalLevel.IsAvailable() && FColletorUnitInfo::StaticStruct(); }

    // 21.40's name, server only: rolls each input's OutputItemEntry from the rarity's loot tier group where the tables
    // have one, else its own (the input's OverrideOutputItemLootTierGroupName, else DefaultItemLootTierGroupName) at the
    // StartingGoalLevel loot tier, so the three inputs of a vending machine sell three items of its rarity.
    void FinishItemSelectionSetup();
    // 21.40's name, server only: what the collector gives a player who reached the goal of InputItem's input, as
    // pickups out of its front (LootSpawnLocation, LootTossDirection).
    void GrantOutput(AFortPlayerController* PlayerController, UFortWorldItemDefinition* InputItem);

    // The timer delegate CheckDepositGoalReached sets (its lambda: the collector, the controller and the input, all
    // weak) is left with no GrantOutput call; its instance's Execute and ExecuteIfSafe slots become Core's.
    static void HookVTables();

private:
    static void GrantOutputDelegateExecuteHK(void* DelegateInstance);
    static bool GrantOutputDelegateExecuteIfSafeHK(void* DelegateInstance);
};
