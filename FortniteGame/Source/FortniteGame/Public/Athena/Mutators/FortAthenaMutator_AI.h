#pragma once

#include "Containers/Array.h"
#include "GameFramework/Info.h"
#include "Reflection.h"

class AFortGameStateAthena;
class UFortMovementComp_AIChar;
class UFortMovementComp_Character;

// The AI support mutator (from 6.20, Fortnitemares' Mutator_AISupport, GameplayMod_DeimosAI): an AFortGameplayMutator in
// the game (Core declares none, so this derives from its AInfo). While it is active the Athena game state hands it the
// Cube Monsters' character movement (AFortGameStateAthena's Add/RemoveManagedCharacterMovementComponent, 6.22 0xC6F230 and
// 0xC8D2A0), and its FortGameStateAthena_TickActor (6.22 0xD15B20, from the game state's TickActor override 0xC97600)
// moves them all at once: batched CalcVelocity, one navmesh projection for every agent's floor, then
// ProjectToGeometry's capsule sweep along each move ("TraceCollisionOnPath"), which stops an agent at a building it
// would walk into and has its AAthenaAIController smash it (6.22 0xA9AD90), then avoidance.
class AFortAthenaMutator_AI : public AInfo
{
    DefineUnrealClass(AFortAthenaMutator_AI, FortniteGame)

public:
    DefineUProperty(TArray<UFortMovementComp_AIChar*>, DeimosMoveComponents)
    static bool HasDeimosMoveComponents() { return StaticClass() && Member_DeimosMoveComponents.IsAvailable(); }

    // Stripped in 6.20 to 6.22 (the shared `xor al, al; ret`), so every Cube Monster went to AFortGameState's list,
    // whose tick (6.22 0xF49AC0) runs the component's own TickComponent: NavWalking, whose capsule ignores WorldStatic,
    // with nothing that stops it at a wall the navmesh doesn't have yet. True when the mutator takes the component.
    bool FortGameStateAthena_AddManagedCharacterMovementComponent(AFortGameStateAthena* GameState, UFortMovementComp_Character* MovementComponent);
    // True when the component was the mutator's (it is then no longer in DeimosMoveComponents).
    bool FortGameStateAthena_RemoveManagedCharacterMovementComponent(AFortGameStateAthena* GameState, UFortMovementComp_Character* MovementComponent);

    // The game state's calls (AFortGameStateAthena::HookCallSites redirects them here): this, the game state, the component.
    static bool AddManagedCharacterMovementComponentHK(AFortAthenaMutator_AI* This, AFortGameStateAthena* GameState, UFortMovementComp_Character* MovementComponent);
    static bool RemoveManagedCharacterMovementComponentHK(AFortAthenaMutator_AI* This, AFortGameStateAthena* GameState, UFortMovementComp_Character* MovementComponent);
};
