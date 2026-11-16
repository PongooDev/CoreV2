#pragma once

#include "AI/Navigation/NavigationSystem.h"
#include "Math/Box.h"
#include "UObject/WeakObjectPtr.h"

class AFortNavigationGraph;

// The engine's FNavigationBounds (0x2C): a box the navigation data builds in, the agents it is for (FNavAgentSelector's
// packed bits, every agent by default) and the level it belongs to. UNavigationSystemV1::GatherNavigationBounds makes one
// per ANavMeshBoundsVolume; UFortNavSystem keeps one more of its own, GridNavBounds (21.40's name).
struct FNavigationBounds
{
    uint32_t UniqueID = 0;
    FBox AreaBox;
    uint32_t SupportedAgents = 0x7FFFFFFF;
    FWeakObjectPtr Level;
};

static_assert(sizeof(FNavigationBounds) == 0x2C);

class UFortNavSystem : public UNavigationSystem
{
    DefineUnrealClass(UFortNavSystem, FortniteGame)
    DefineUFunction(OnNavDataRegistered)

public:
    DefineUProperty(AFortNavigationGraph*, NavGraphData)

    void OnNavDataRegistered(ANavigationData* NavData);

    // The game's own (21.40's name, a virtual; 6.22 CL-4543176 0xB1D140, 6.30 0xB41960, 6.31 0xB433E0, the function that
    // logs "Auto generating navigation bounds: %s"). It keeps Bounds as GridNavBounds, which its GatherNavigationBounds
    // override (6.31 slot 0x57, 0xB36A20) adds to the volumes' bounds; spawns the agents' navigation data when there is
    // none, and the graph when there is no graph; unlocks the navigation octree and populates it; runs the graph's
    // OnWorldInitialized (which returns at once when the graph's cells are laid out, as Core's are); releases the initial
    // building lock, which rebuilds the navmeshes inside the bounds; and gives the main navmesh (an AFortNavMesh) the AI
    // system's hot spot manager. Its caller, the zone's world-ready step, is stripped. False where the function isn't
    // there intact (1.1's body is stripped: only its log string is left).
    bool PrepareBoundsAndUnlock(const FNavigationBounds& Bounds);
};
