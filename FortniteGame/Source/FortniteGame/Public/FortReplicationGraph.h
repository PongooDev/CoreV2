#pragma once

#include "ReplicationGraph.h"

class AActor;
class UNetConnection;

// One viewer's entry in the node's PastRelevantActors (9.00, 9.01; 21.40 has the same struct with LastTetherPawn after it).
struct FFortAlwaysRelevantActorInfo
{
    UNetConnection* Connection;
    AActor* LastPawn;
};

// UFortReplicationGraph's always relevant node of one connection: the player controller, its player state, its pawn
// and what that pawn carries. Its GatherActorListsForConnection (9.00 0x16BB1F0) sets the connection's info for a new
// pawn to cull distance 0 ("Setting connection pawn cull distance to 0") only when the pawn isn't the viewer's entry's
// LastPawn: that is what makes a player's own pawn reach them wherever their view point is.
class UFortReplicationGraphNode_AlwaysRelevant_ForConnection : public UReplicationGraphNode
{
    DefineUnrealClass(UFortReplicationGraphNode_AlwaysRelevant_ForConnection, FortniteGame)

public:
    // CoreV2 #671. 9.00's LastPawn is a raw pointer nothing clears, so it outlives the pawn: the warmup pawn
    // StartAircraftPhase destroys stays the entry's LastPawn through the flight, and a pawn spawned at its address is
    // taken for it. Core's server collects garbage during a match (Epic's had bDisableGCOnServerDuringMatch, see
    // UReplicationGraph::HandleActorSpawned), so the pawn a player leaves the bus with at the end of the drop zone
    // (OnAircraftExitedDropZone's RestartPlayer) is often spawned in the warmup pawn's memory. Its cull distance stays
    // the class's, the server's view point for that player is still the warmup island's (the client's last camera
    // update; it sends none without a pawn), and the pawn is never sent: the player stays in the bus. 21.40's
    // FFortAlwaysRelevantActorInfo is a struct whose pointers are reported to the garbage collector
    // (TStructOpsTypeTraits AddStructReferencedObjects), which nulls them once the pawn is destroyed. Called when a match
    // pawn is destroyed (AFortGameModeAthena::HandlePawnDestroyed), this does the same for each connection's node.
    // Returns how many entries let the pawn go, or -1 where the build's node isn't 9.00's or 9.01's
    // (Finder::ResolveAlwaysRelevantPastActorsLive).
    static int32_t ForgetDestroyedPawn(UReplicationGraph* Graph, AActor* Pawn);
};
