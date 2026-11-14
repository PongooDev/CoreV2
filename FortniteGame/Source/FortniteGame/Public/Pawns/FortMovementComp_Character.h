#pragma once

#include "GameFramework/CharacterMovementComponent.h"
#include "Natives.h"
#include "ObjectLayout.h"

#include <cstdint>

// Fortnite's character movement, every Fortnite pawn's.
class UFortMovementComp_Character : public UCharacterMovementComponent
{
    DefineUnrealClass(UFortMovementComp_Character, FortniteGame)

public:
    // From 6.20 (21.40's name): the component's place in the list that ticks it, AFortGameState's
    // ManagedCharMovementComponents (AFortGameState::AddManagedCharacterMovementComponent stores it, Remove's
    // RemoveAtSwap reads it and leaves -1) or the AI mutator's DeimosMoveComponents. Unreflected; available once
    // Finder::ResolveManagedCharacterMovementLive found it.
    DefineNativeMember(int32_t, ManagedTickIndex, UFortMovementComp_Character_ManagedTickIndex)
    static bool HasManagedTickIndex() { return Natives::UFortMovementComp_Character_ManagedTickIndex > 0; }
};

// The AI pawns' character movement (from 6.20 on the Fortnitemares Cube Monsters' pawns: Deimos_Fiend's and
// Deimos_Brute's CharMoveComp have bDeimos).
class UFortMovementComp_AIChar : public UFortMovementComp_Character
{
    DefineUnrealClass(UFortMovementComp_AIChar, FortniteGame)

public:
    // The Cube Monsters' movement: NavWalking where the base class would walk (6.22's SetMovementMode override 0x10BBE60
    // turns MOVE_Walking into MOVE_NavWalking), and ticked with the others in the AI mutator's batch
    // (AFortAthenaMutator_AI::FortGameStateAthena_AddManagedCharacterMovementComponent).
    DefineBitfieldUProperty(bDeimos)
    static bool HasDeimos() { return StaticClass() && Member_bDeimos.IsAvailable(); }
};
