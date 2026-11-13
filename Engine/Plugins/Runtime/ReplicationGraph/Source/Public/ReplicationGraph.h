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
    // which UE 4.21's (6.00 0x7DE420, 6.01 to 6.10 the same, 6.20 0x74A3D0) doesn't have: there a connection's info
    // for an actor stays in its ActorMap, keyed by the actor's address, after the actor is gone. A torn off actor's keeps bTearOff,
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
    // Returns how many connections had an entry, or -1 where nothing was done: a build whose code isn't what 6.00 to 6.20
    // have (Finder::ResolveReplicationGraphActorInfoLive) keeps its maps untouched.
    int32_t RemoveActorInfoFromConnections(AActor* Actor);

    // Core's (CoreV2 #412): the same step for each actor the world spawns, on a server whose net driver has a
    // replication graph (UWorld::Listen adds it to the world's OnActorSpawned). The engine's RemoveNetworkActor, where a
    // later engine forgets an actor, is the build's own intact code, which Core doesn't hook, and the engine broadcasts
    // nothing when an actor is destroyed but that actor's own OnDestroyed. It does broadcast every spawn, as SpawnActor's
    // last act, before anything can have replicated the new actor: whatever a connection still holds at a new actor's
    // address is a destroyed actor's, and goes then.
    //
    // A torn off actor's info is one kind that does harm (#411). A dormant actor's is the other: an actor destroyed
    // while it was dormant on a connection leaves bDormantOnConnection set (+0x1D bit 0, +0x28 on 6.20, set by
    // NotifyActorFullyDormantForConnection, 6.00 0x7D59B0, 6.02 0x78B6B0, 6.20 0x741940; cleared only by that actor's
    // own dormancy flush, FlushNetDormancy 0x7CEDC0, and only for an actor that wants to be dormant), and the graph skips an actor
    // whose info has it before any other check (6.00 0x7DE8AC, 6.02 0x7945AC). An actor that is never dormant, spawned
    // at that address, is never sent to that connection at all, and nothing about it is in either log.
    //
    // Every pickup is DORM_DormantAll (AFortPickup's constructor, 6.00 0xFCB280) and an AFortPickupAthena is 0x5C0
    // bytes, the same block size of the engine's allocator as the storm circle (SafeZoneIndicator_C, 0x5F0 on 6.00 and
    // 0x600 on 6.02). So a storm circle spawned in the memory of a pickup a player had seen go never reached that
    // player: no storm on their screen or map, and the timer under the minimap at 0:00 (#412; on 50v50, whose circle
    // comes during the bus, with the bus's icon).
    //
    // Epic's servers never met either: AFortGameModeAthena's config bDisableGCOnServerDuringMatch is true (read on
    // 6.02), so they collected no garbage during a match and no actor's address was used twice in one. What reads it
    // went with the stripped server code (the build only sets and restores it: the playlist's GCSettingsOverrideList,
    // 6.00 0xC28790 and 0xC14800), and a Core server collects every gc.TimeBetweenPurgingPendingKillObjects (61 s).
    static void HandleActorSpawned(AActor* Actor);
};
