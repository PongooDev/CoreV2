#pragma once

#include "Math/Box.h"
#include "Math/Vector.h"

class AActor;
class ANavigationData;
class UWorld;

// Core's backport of 8.30's Battle Royale navigation (UAthenaNavSystem, UAthenaNavSystemConfig, CoreV2 #233): 3.0's
// Athena_Terrain has no navmesh, no NavMeshBoundsVolume and no nav data (Athena-Bots-Plan.md 2.4), and a full-island
// build would stall the server (the Zone freezes, #16 / #29), so the navmesh is generated at runtime only in the tiles
// around navigation invokers, as 8.30's bUseNavigationInvokers does: each player bot's pawn is one.
//
// 3.0 has no UAthenaNavSystem, and Core can't add UClasses: this is a Core type named after Epic's, driving the world's
// own UNavigationSystem (UFortNavSystem) through its reflected members and UFunctions. Nothing is hooked.
class UAthenaNavSystem
{
public:
    // Once per match, when the bots need navigation: the invoker-only flag, one NavMeshBoundsVolume over the island,
    // the octree unlocked and populated, a navmesh for the player-sized agent, and the initial build lock released (its
    // RebuildAll only marks the invokers' tiles). False when the build's navigation can't be set up this way; the bots
    // steer without it.
    static bool InitializeForBots(UWorld* World, const FBox& IslandBounds);
    static bool IsInitialized();
    // A new match (its world is new: the server reloads the map after a match): the last world's navigation is gone
    // with it (UWorld::CleanupWorld cleans up its navigation system), so the next match sets its own up.
    static void ResetForNewMatch();
    // Each frame: once the navmesh is registered, the octree is populated and the initial build lock released.
    static void Tick(UWorld* World);

    // The navmesh the bots path on, once registered with the navigation system.
    static ANavigationData* GetNavData(UWorld* World);

    // 8.30's AAthenaNavInvokerBox / RegisterNavGenerationObserver: navigation is generated around Invoker while it lives.
    static void RegisterNavigationInvoker(AActor* Invoker);

    // Whether there is navmesh under Location (its tile is built).
    static bool IsOnNavMesh(UWorld* World, const FVector& Location);
};
