#pragma once

#include "GameFramework/Controller.h"
#include "Math/Vector.h"

class AActor;

// EPathFollowingRequestResult::Type: what MoveToLocation did with the request.
namespace EPathFollowingRequestResult
{
    enum Type : uint8_t
    {
        Failed,
        AlreadyAtGoal,
        RequestSuccessful,
    };
}

// The engine's AI controller (AIModule). Battle Royale's player bots are stock AAIControllers, as Epic's 8.30
// FCoreAthenaBotController derives from it rather than from AFortAIController (which wants an AFortAIPawn).
class AAIController : public AController
{
    DefineUnrealClass(AAIController, AIModule)
    DefineUFunction(MoveToLocation)
    DefineUFunction(K2_SetFocalPoint)
    DefineUFunction(K2_SetFocus)
    DefineUFunction(K2_ClearFocus)
    DefineUFunction(GetMoveStatus)
    DefineUFunction(StopMovement)

public:
    // Whether the controller gets a player state of the game mode's PlayerStateClass (AAIController::PostInitializeComponents
    // calls InitPlayerState then), so it must be set before the controller finishes spawning.
    DefineBitfieldUProperty(bWantsPlayerState)
    DefineBitfieldUProperty(bStopAILogicOnUnposses)

    // Moves the pawn to Dest; without bUsePathfinding it goes in a straight line (no navigation data needed).
    EPathFollowingRequestResult::Type MoveToLocation(const FVector& Dest, float AcceptanceRadius = -1.0f, bool bStopOnOverlap = true, bool bUsePathfinding = true,
        bool bProjectDestinationToNavigation = false, bool bCanStrafe = true, bool bAllowPartialPath = true);
    // The point the controller (and its pawn) turns to: UpdateControlRotation aims the control rotation at it each tick.
    void SetFocalPoint(const FVector& FocalPoint);
    void SetFocus(AActor* NewFocus);
    void ClearFocus();
    // EPathFollowingStatus: 0 Idle, 1 Waiting, 2 Paused, 3 Moving.
    uint8_t GetMoveStatus() const;
    // AController::StopMovement: the path following stops.
    void StopMovement();
};
