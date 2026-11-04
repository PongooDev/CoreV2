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
//
// The navmesh's tiles are cached per build, map and agent (Core's, CoreV2 #233; 26.20 cooks the island's navmesh
// instead): each tile the engine builds for the base map is saved to a file beside Core.dll, the next matches add the
// saved tiles to their navmesh (dtNavMesh::addTile) and keep the ones around the bots in the generator's active tiles,
// so the engine only builds tiles nobody has saved yet, and the ones a structure changed (its own dirty areas).
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
    // with it (UWorld::CleanupWorld cleans up its navigation system), so the next match sets its own up. The cached
    // tiles stay in memory for it.
    static void ResetForNewMatch();
    // Each frame: once the navmesh is registered, the octree is populated, the initial build lock released and the tile
    // cache's file read on a worker thread; then the tile cache: cached tiles added, the bots' cached tiles kept active,
    // new tiles saved.
    static void Tick(UWorld* World);

    // A structure was built, edited or destroyed at Actor (Core's building and harvesting code): the cached tiles under
    // it are out of date for this match, so the engine builds them again when a bot comes near, and none of them is
    // saved as the base map's. Near a bot the engine's own dirty area rebuilds them anyway.
    static void AddDirtyArea(const AActor* Actor);

    // The navmesh the bots path on, once registered with the navigation system.
    static ANavigationData* GetNavData(UWorld* World);

    // 8.30's AAthenaNavInvokerBox / RegisterNavGenerationObserver: navigation is generated around Invoker while it lives.
    static void RegisterNavigationInvoker(AActor* Invoker);

    // UNavigationSystem::ProjectPointToNavigation on the bots' navmesh: the nearest navmesh point within Extent of Point,
    // or false when there is none there (no tile built yet, or inside geometry).
    static bool ProjectPointToNavigation(UWorld* World, const FVector& Point, FVector& OutLocation, const FVector& Extent);
    // Whether there is navmesh under Location (its tile is built).
    static bool IsOnNavMesh(UWorld* World, const FVector& Location);
};
