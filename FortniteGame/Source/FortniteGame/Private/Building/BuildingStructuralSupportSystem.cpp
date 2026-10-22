#include "pch.h"
#include "Building/BuildingStructuralSupportSystem.h"
#include "AI/Navigation/FortNavigationGraph.h"
#include "Engine/World.h"
#include "Hooks.h"
#include "Math/Vector.h"
#include "Natives.h"

namespace
{
    void (*InitializeGridOG)(UBuildingStructuralSupportSystem* This, const FVector* GridCenter, int32_t TileCells, int32_t TilesX, int32_t TilesY, int32_t CellsZ) = nullptr;
}

bool UBuildingStructuralSupportSystem::GetGrid(FVector& OutOrigin, int32_t OutSize[3]) const
{
    if (Natives::UBuildingStructuralSupportSystem_GridSize < 0 || Natives::UBuildingStructuralSupportSystem_GridOrigin < 0)
    {
        return false;
    }
    const uint8_t* Base = reinterpret_cast<const uint8_t*>(this);
    const int32_t* Size = reinterpret_cast<const int32_t*>(Base + Natives::UBuildingStructuralSupportSystem_GridSize);
    OutOrigin = *reinterpret_cast<const FVector*>(Base + Natives::UBuildingStructuralSupportSystem_GridOrigin);
    for (int32_t Axis = 0; Axis < 3; ++Axis)
    {
        OutSize[Axis] = Size[Axis];
    }
    return OutSize[0] > 0 && OutSize[1] > 0 && OutSize[2] > 0;
}

void UBuildingStructuralSupportSystem::InitializeGrid(const FVector* GridCenter, int32_t TileCells, int32_t TilesX, int32_t TilesY, int32_t CellsZ)
{
    InitializeGridOG(this, GridCenter, TileCells, TilesX, TilesY, CellsZ);

#if WITH_SERVER_CODE
    FVector GridOrigin;
    int32_t GridSize[3] = {};
    if (!NavGraph && GetGrid(GridOrigin, GridSize))
    {
        UWorld* World = GetWorld();
        if (AFortNavigationGraph* Graph = AFortNavigationGraph::FindOrSpawn(World ? World : static_cast<UWorld*>(GWorld)))
        {
            Graph->InitializeCells(this);
        }
    }
#endif
}

void UBuildingStructuralSupportSystem::Hook()
{
    if (!AFortNavigationGraph::ResolveNatives())
    {
        return;
    }
    Hooks::Detour(L"UBuildingStructuralSupportSystem::InitializeGrid", reinterpret_cast<void*>(Natives::UBuildingStructuralSupportSystem_InitializeGrid), &InitializeGridHK, &InitializeGridOG);
}

void UBuildingStructuralSupportSystem::InitializeGridHK(UBuildingStructuralSupportSystem* This, const FVector* GridCenter, int32_t TileCells, int32_t TilesX, int32_t TilesY, int32_t CellsZ)
{
    This->InitializeGrid(GridCenter, TileCells, TilesX, TilesY, CellsZ);
}
