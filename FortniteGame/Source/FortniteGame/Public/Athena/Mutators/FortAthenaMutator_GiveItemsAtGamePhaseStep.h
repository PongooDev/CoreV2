#pragma once

#include "Athena/Mutators/FortAthenaMutator.h"
#include "GameplayEffect.h"
#include "Reflection.h"
#include "UObject/Stack.h"

class UFortWorldItemDefinition;

// One item a give-items mutator hands each player: NumberToGive of ItemToDrop.
struct FItemsToGive
{
    DefineUnrealStruct(FItemsToGive, FortniteGame)
    DefineUProperty(UFortWorldItemDefinition*, ItemToDrop)
    DefineUStructProperty(FScalableFloat, NumberToGive)
};

// Gives every player ItemsToGive when the game state's GamePhaseStep reaches PhaseToGiveItems (from 8.00; its
// constructor's default is BusFlying, 8.51 CL-6165369 0x104D0C0). 8.51's users: Food Fight's BigTeamGliderModifier
// (Mutator_GiveBigTeamModeGlider_C, the big team glider), Deep Fried's BarrierModifier_B_Lava (Barrier's
// Mutator_GiveItems_C, the grappler), Slide's and Hover's grapplers, and Gun Game's Mutator_GrantInitialWeapon_C (the
// first weapons, ammo and materials).
//
// BeginPlay is intact (8.51 0x1056350, slot 0x60): on the authority, while the game state's step is below
// PhaseToGiveItems, it binds OnGamePhaseStepChanged to the game state's GamePhaseStepChanged. The UFunction's native is
// stripped: its exec (8.51 0x19650E0, 8.40 0x18B34D0, 8.30 0x1892510) is the one AFortAthenaMutator_GiveItemsAtGamePhase's
// OnGamePhaseChanged has too, and steps over the step to tail-call an empty function. So no item was ever given.
class AFortAthenaMutator_GiveItemsAtGamePhaseStep : public AFortAthenaMutator
{
    DefineUnrealClass(AFortAthenaMutator_GiveItemsAtGamePhaseStep, FortniteGame)
    DefineUFunction(OnGamePhaseStepChanged)

public:
    DefineUProperty(uint8_t, PhaseToGiveItems)
    DefineUStructArrayProperty(FItemsToGive, ItemsToGive)
    static bool HasItemsToGive() { return Member_PhaseToGiveItems.IsAvailable() && Member_ItemsToGive.IsAvailable() && FItemsToGive::StaticStruct(); }

    void OnGamePhaseStepChanged(uint8_t GamePhaseStep);

    DECLARE_FUNCTION(execOnGamePhaseStepChanged);

    // Restores both give-items mutators' natives where the build stripped them.
    static void HookNatives();
};
