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

class UFortSchematicItem : public UFortItem
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
    int32_t ServerCraftSchematic(AFortPlayerController* PlayerController, int32_t Count);

    static void HookVTables();

private:
    static int32_t ServerCraftSchematicHK(UFortSchematicItem* This, AFortPlayerController* PlayerController, int32_t Count);
};
