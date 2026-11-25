#include "pch.h"
#include "Athena/Mutators/FortAthenaMutator_SpecialEvent.h"

void AFortAthenaMutator_SpecialEvent::FortGameStateAthena_ModifyNamedWeights(TMap<FName, float>& NamedWeights) const
{
    // SetNamedWeightForContainerLootRoll and SetWeightForContainerLootRoll (8.51 0x196D7A0, 0x196D8A0) only store them.
    if (!HasContainerLootRollWeight())
    {
        return;
    }
    const FName Name = NamedWeightForContainerLootRoll;
    if (!Name.IsNone())
    {
        NamedWeights.FindOrAdd(Name) = WeightForContainerLootRoll;
    }
}
