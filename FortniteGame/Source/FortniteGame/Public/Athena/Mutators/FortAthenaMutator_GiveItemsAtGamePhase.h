#pragma once

#include "Athena/Mutators/FortAthenaMutator.h"
#include "GameplayEffect.h"
#include "Reflection.h"
#include "UObject/Stack.h"

class UFortWorldItemDefinition;

// One item AFortAthenaMutator_GiveItemsAtGamePhase hands each player: NumberToGive of ItemToDrop.
struct FItemsToGiveAtPhase
{
    DefineUnrealStruct(FItemsToGiveAtPhase, FortniteGame)
    DefineUProperty(UFortWorldItemDefinition*, ItemToDrop)
    DefineUStructProperty(FScalableFloat, NumberToGive)
};

// Gives every player ItemsToGive when the game state's GamePhase reaches PhaseToGiveItems (from 8.00). 8.51's user:
// Air Royale's Mutator_GiveItemsAtPhase_C (Aircraft: its pistols, rifle and ammo). BeginPlay is intact (8.51 0x1056200)
// and binds OnGamePhaseChanged to the game state's GamePhaseChanged while the phase is below PhaseToGiveItems; the
// native is stripped as AFortAthenaMutator_GiveItemsAtGamePhaseStep's is (the same exec).
class AFortAthenaMutator_GiveItemsAtGamePhase : public AFortAthenaMutator
{
    DefineUnrealClass(AFortAthenaMutator_GiveItemsAtGamePhase, FortniteGame)
    DefineUFunction(OnGamePhaseChanged)

public:
    DefineUProperty(uint8_t, PhaseToGiveItems)
    DefineUStructArrayProperty(FItemsToGiveAtPhase, ItemsToGive)
    static bool HasItemsToGive() { return Member_PhaseToGiveItems.IsAvailable() && Member_ItemsToGive.IsAvailable() && FItemsToGiveAtPhase::StaticStruct(); }

    void OnGamePhaseChanged(uint8_t GamePhase);

    DECLARE_FUNCTION(execOnGamePhaseChanged);
};
