#pragma once

#include "Items/FortItem.h"
#include "StructArray.h"

class AFortPlayerController;
class UFortItemDefinition;

struct FSchematicRequirement
{
    DefineUnrealStruct(FSchematicRequirement, FortniteGame)
    DefineUProperty(UFortItemDefinition*, ItemDefinition)
    DefineUProperty(int32_t, Count)
    DefineUProperty(bool, bIsConsumed)
};

class UFortSchematicItem : public UFortItem
{
    DefineUnrealClass(UFortSchematicItem, FortniteGame)
    DefineUFunction(GetSchematicRequirements)
    DefineUFunction(GetSchematicQuantityProduced)

public:
    void GetSchematicRequirements(TStructArray<FSchematicRequirement>& Requirements) const;
    int32_t GetSchematicQuantityProduced() const;
    bool CanCraftSchematic(AFortPlayerController* PlayerController, uint8_t& OutFailCause, bool bConsiderReservedItems, int32_t Count);
    int32_t ServerCraftSchematic(AFortPlayerController* PlayerController, int32_t Count);

    static void HookVTables();

private:
    static int32_t ServerCraftSchematicHK(UFortSchematicItem* This, AFortPlayerController* PlayerController, int32_t Count);
};
