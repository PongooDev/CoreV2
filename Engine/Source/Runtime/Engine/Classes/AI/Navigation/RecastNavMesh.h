#pragma once

#include "AI/Navigation/NavigationData.h"
#include "Natives.h"

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
    // The tiles' size and voxels, and the agent they are built for: what a tile's data depends on.
    DefineUProperty(float, TileSizeUU)
    DefineUProperty(float, CellSize)
    DefineUProperty(float, CellHeight)
    DefineUProperty(float, AgentRadius)
    DefineUProperty(float, AgentHeight)
    static bool HasTileSettings() { return Member_TileSizeUU.IsAvailable() && Member_CellSize.IsAvailable() && Member_CellHeight.IsAvailable() && Member_AgentRadius.IsAvailable() && Member_AgentHeight.IsAvailable(); }

    // Detour's navmesh (dtNavMesh): FPImplRecastNavMesh's DetourNavMesh (RecastNavMeshImpl), once the generator has made
    // it. Needs Finder::ResolveRecastNavMeshLive.
    void* GetRecastMesh() const
    {
        if (Natives::ARecastNavMesh_RecastNavMeshImpl <= 0 || Natives::FPImplRecastNavMesh_DetourNavMesh < 0)
        {
            return nullptr;
        }
        const uint8_t* Impl = *reinterpret_cast<const uint8_t* const*>(reinterpret_cast<const uint8_t*>(this) + Natives::ARecastNavMesh_RecastNavMeshImpl);
        return Impl ? *reinterpret_cast<void* const*>(Impl + Natives::FPImplRecastNavMesh_DetourNavMesh) : nullptr;
    }
};
