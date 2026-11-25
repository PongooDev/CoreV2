#pragma once

#include "Athena/Mutators/FortAthenaMutator.h"
#include "Containers/Map.h"
#include "Reflection.h"
#include "UObject/NameTypes.h"

// The live events' mutator (Mutator.SpecialEvent: Athena_GameMode's SpecialEventModifier, from 7.20). 8.51's Unvaulting
// (BP_SnowScripting's SetLootWeights, on OnRep_UnvaultedItemName) gives it the vote's winner as a named weight for the
// containers' loot: NamedWeightForContainerLootRoll (GW.DrumGun) and WeightForContainerLootRoll (AthenaGameData's
// GW.DrumGun row, 0.85), which 8.51's AthenaLootPackages_Client rows name in their NamedWeightMult (the Drum Gun's
// WorldList.AthenaLoot.Weapon.AssaultAuto rows, Weight 0).
class AFortAthenaMutator_SpecialEvent : public AFortAthenaMutator
{
    DefineUnrealClass(AFortAthenaMutator_SpecialEvent, FortniteGame)

public:
    DefineUProperty(FName, NamedWeightForContainerLootRoll)
    DefineUProperty(float, WeightForContainerLootRoll)

    static bool HasContainerLootRollWeight() { return StaticClass() && Member_NamedWeightForContainerLootRoll.IsAvailable() && Member_WeightForContainerLootRoll.IsAvailable(); }

    // 21.40's mutator event of the name (AFortAthenaMutator_WarEffort's): the mutator's named weight for the loot rolls.
    // 8.51 reads its two fields nowhere (the rolls are server code): Core's loot rolls ask the game state for them.
    void FortGameStateAthena_ModifyNamedWeights(TMap<FName, float>& NamedWeights) const;
};
