#pragma once

#include "GameFramework/Pawn.h"
#include "Math/Vector.h"

// The physics state a client sends with a physics ball hit (FTimeStampedPhysicsPawnState, 9.00+: 0x50 bytes, WorldTime
// then FReplicatedPhysicsPawnState at +0x10). Core only passes it on.
struct alignas(16) FTimeStampedPhysicsPawnState
{
    float WorldTime = 0.0f;
    uint8_t Pad[0x4C] = {};
};
static_assert(sizeof(FTimeStampedPhysicsPawnState) == 0x50);

// Creative's rigid bodies (9.00+; the ball device's CreativeBall_C : ARigidBall : APhysicsObject : AFortPhysicsPawn).
// The server simulates the body and its tick writes CurrentServerPhysicsState (9.21 0x11F48A0), which clients correct
// towards (OnRep_CurrentServerPhysicsState, 0x11D1C70).
class APhysicsObject : public APawn
{
    DefineUnrealClass(APhysicsObject, FortniteGame)
    DefineUFunction(ApplyImpulseFromPawnHit)

public:
    // Queues the impulse of a pawn hit (9.21 0x11D25C0); the object's tick (0x11EE920) adds the queued impulses to its
    // PhysicsMeshComponent.
    void ApplyImpulseFromPawnHit(const FVector& VelocityOfHitter, const FVector& NormalTowardObject, APawn* HittingPawn);
};
