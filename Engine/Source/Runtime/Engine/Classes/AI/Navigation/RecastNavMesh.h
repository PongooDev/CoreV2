#pragma once

#include "AI/Navigation/NavigationData.h"

// The engine's recast navmesh (Fortnite's FortNavMesh derives from it). Its generator (FRecastNavMeshGenerator::Init)
// reads these when it is made, at the navmesh's registration.
class ARecastNavMesh : public ANavigationData
{
    DefineUnrealClass(ARecastNavMesh, Engine)

public:
    // Whether a tile's geometry is gathered on its worker thread (FRecastTileGenerator::DoAsyncGeometryGathering) rather
    // than on the game thread when the tile's job is made (GatherGeometry). The engine's default is off.
    DefineBitfieldUProperty(bDoFullyAsyncNavDataGathering)
    // How many tile jobs run at once (the engine's default is 1024).
    DefineUProperty(int32_t, MaxSimultaneousTileGenerationJobsCount)
    static bool HasTileGenerationSettings() { return Member_bDoFullyAsyncNavDataGathering.IsAvailable() && Member_MaxSimultaneousTileGenerationJobsCount.IsAvailable(); }
};
