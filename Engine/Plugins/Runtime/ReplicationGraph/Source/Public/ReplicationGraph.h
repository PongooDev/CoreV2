#pragma once

#include "Containers/Array.h"
#include "Engine/ReplicationDriver.h"

class AActor;

// UE 4.20's replication graph, the replication driver of Fortnite's Battle Royale from 3.4 (UFortReplicationGraph).
// The build's own code runs it; Core only declares what it reads.
class UReplicationGraph : public UReplicationDriver
{
    DefineUnrealClass(UReplicationGraph, ReplicationGraph)

public:
    // A UNetReplicationGraphConnection for each client connection the graph replicates to.
    DefineUProperty(TArray<UObject*>, Connections)
    static bool HasConnections() { return StaticClass() && Member_Connections.IsAvailable(); }

    // The last step of a later engine's UReplicationGraph::RemoveNetworkActor (8.30 0x8E1B80, from 0x8E1C70):
    //     for (UNetReplicationGraphConnection* ConnectionManager : Connections)
    //         ConnectionManager->ActorInfoMap.RemoveActor(Actor);      // ActorMap.Remove(Actor), 0x38A1C0
    // which UE 4.21's (6.00 0x7DE420, 6.01 and 6.02 the same) doesn't have: there a connection's info for an actor
    // stays in its ActorMap, keyed by the actor's address, after the actor is gone. A torn off actor's keeps bTearOff,
    // and UReplicationGraph::ReplicateSingleActor (6.00 0x7DF520) closes the channel right after replicating whenever
    // that is set: an actor spawned at that address later is opened and closed on that connection every time it
    // replicates (CoreV2 #411: a Playground respawn's pawn at a dead pawn's address never reached its player).
    //
    // Called when an actor is destroyed, where the engine's own RemoveNetworkActor runs (UNetDriver::NotifyActorDestroyed,
    // in the same UWorld::DestroyActor), with the build's own remove-by-key of that map (the one
    // UNetReplicationGraphConnection::NotifyActorChannelCleanedUp calls on the ChannelMap beside it). An info whose
    // channel is still closing stays alive in the ChannelMap, which shares it, until the channel is cleaned up, as in
    // 8.30. Like 8.30's, Connections only, not PendingConnections.
    //
    // Returns how many connections had an entry, or -1 where nothing was done: a build whose code isn't what 6.00 to 6.02
    // have (Finder::ResolveReplicationGraphActorInfoLive) keeps its maps untouched.
    int32_t RemoveActorInfoFromConnections(AActor* Actor);
};
