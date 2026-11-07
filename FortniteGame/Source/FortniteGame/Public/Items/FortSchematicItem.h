#pragma once

#include "Items/FortItem.h"
#include "StructArray.h"
#include "UObject/AssetPtr.h"

class AFortPlayerController;
class UFortItemDefinition;

namespace Reflection
{
    class FParams;
}

struct FSchematicRequirement
{
    DefineUnrealStruct(FSchematicRequirement, FortniteGame)
    DefineUProperty(UFortItemDefinition*, ItemDefinition)
    DefineUProperty(int32_t, Count)
    DefineUProperty(bool, bIsConsumed)
};

// An item and how many of it (from 1.11), a recipe's cost or result: the item is a soft pointer.
struct FFortItemQuantityPair
{
    DefineUnrealStruct(FFortItemQuantityPair, FortniteGame)
    DefineUProperty(TAssetPtr<UFortItemDefinition>, ItemDefinition)
    DefineUProperty(int32_t, Quantity)
};

// A schematic's recipe (from 1.11, an FTableRowBase), which replaces 1.10's FSchematicRequirement list: what it costs
// (RecipeCosts), bIsConsumed (one flag for the recipe, which crafting doesn't read: every cost is taken) and what it makes.
struct FRecipe
{
    DefineUnrealStruct(FRecipe, FortniteGame)
    DefineUStructArrayProperty(FFortItemQuantityPair, RecipeResults)
    DefineUProperty(bool, bIsConsumed)
    DefineUStructArrayProperty(FFortItemQuantityPair, RecipeCosts)
};

// An account item with perks (a schematic). Alterations holds the template id of each alteration slot's perk ("" for an
// empty slot), the MCP item's "alterations"; the profile load puts the definition each one names at the same index of
// AlterationInstances (4.2 0xD436A0).
class UFortAlterableItem : public UFortItem
{
    DefineUnrealClass(UFortAlterableItem, FortniteGame)

public:
    // The template ids of the perks GetActiveAlterations returns, in its order: what a crafted item's
    // AlterationDefinitions carries. False when the build has no perks on schematics.
    bool GetActiveAlterationTemplateIds(TArray<FString>& OutTemplateIds, TArray<UObject*>& OutDefinitions) const;

private:
    DefineUProperty(TArray<FString>, Alterations)
    DefineUProperty(TArray<UObject*>, AlterationInstances)
};

class UFortSchematicItem : public UFortAlterableItem
{
    DefineUnrealClass(UFortSchematicItem, FortniteGame)
    DefineUFunction(GetSchematicRequirements)
    DefineUFunction(GetSchematicRecipe)
    DefineUFunction(GetSchematicQuantityProduced)

public:
    // Up to 1.10; 1.11 has GetSchematicRecipe in its place.
    void GetSchematicRequirements(TStructArray<FSchematicRequirement>& Requirements) const;
    // From 1.11. The recipe is returned by value, which Core has no layout for: it stays in Recipe's frame, which
    // destroys it, and the result points there (null when the build has no GetSchematicRecipe).
    FRecipe* GetSchematicRecipe(Reflection::FParams& Recipe) const;
    int32_t GetSchematicQuantityProduced() const;
    bool CanCraftSchematic(AFortPlayerController* PlayerController, uint8_t& OutFailCause, bool bConsiderReservedItems, int32_t Count);
    // The game calls it with the RPC's PostCraftSlot, the primary quick bar slot the crafted item goes to (-1: the first
    // free one), as 2.2's AFortPlayerController::ServerCraftSchematic_Implementation (0x897AD0) does; 21.40's has
    // CraftAmount and RequestedTier after it. Core crafts an ingredient's shortfall with a CraftAmount of its own.
    int32_t ServerCraftSchematic(AFortPlayerController* PlayerController, int32_t PostCraftSlot, int32_t CraftAmount = 1);

    static void HookVTables();

private:
    static int32_t ServerCraftSchematicHK(UFortSchematicItem* This, AFortPlayerController* PlayerController, int32_t PostCraftSlot);
};
