#pragma once

#include "Items/FortItemDefinition.h"
#include "UObject/AssetPtr.h"

class AActor;

// The loot table's stand-in for a building gameplay actor that is picked up in place (Battle Royale's foraged items from
// 4.0: Athena_Foraged_LowGravity is the Hop Rock, CBGA_LowGravity_UsingJump_C): ABGAConsumableSpawner rolls it and
// spawns ConsumableClass, an ABuildingGameplayActorConsumable, in the world.
class UBGAConsumableWrapperItemDefinition : public UFortItemDefinition
{
    DefineUnrealClass(UBGAConsumableWrapperItemDefinition, FortniteGame)

public:
    DefineUProperty(TAssetSubclassOf<AActor>, ConsumableClass)
};
