#pragma once

#include "AI/Navigation/NavigationData.h"
#include "Containers/Array.h"
#include "Natives.h"

#include <cstddef>
#include <cstdint>

class UBuildingStructuralSupportSystem;
class UWorld;

struct FNavGraphCell
{
    uint16_t Block;
    uint16_t FirstLink;
    uint16_t CenterLinks;
    uint16_t GroundSteps;
    uint16_t Reserved8;
    uint8_t RampLinkCount;
    uint8_t FloorLinkCount;
    uint8_t Walls;
    uint8_t RampHeights;
    uint8_t Floors;
    uint8_t GroundLinks;
    uint8_t GroundJumps;
    uint8_t GroundClimbs;
    uint8_t Reserved18;
    uint8_t Reserved19;
};

static_assert(sizeof(FNavGraphCell) == 20);

struct FNavGraphCells
{
    int32_t Origin[3];
    int32_t Size[3];
    int32_t Blocks[3];
    int32_t BlockMask[3];
    int32_t BlockShift[3];
    int32_t NumBlocks;
    int32_t CellsPerBlock;
    int32_t Padding;
    TArray<int32_t> BlockCells;
    TArray<FNavGraphCell> Cells;
};

static_assert(offsetof(FNavGraphCells, NumBlocks) == 0x3C);
static_assert(offsetof(FNavGraphCells, CellsPerBlock) == 0x40);
static_assert(offsetof(FNavGraphCells, BlockCells) == 0x48);
static_assert(offsetof(FNavGraphCells, Cells) == 0x58);

class AFortNavigationGraph : public ANavigationData
{
    DefineUnrealClass(AFortNavigationGraph, FortniteGame)
    DefineNativeObject(FNavGraphCells, Cells, AFortNavigationGraph_Cells)
    DefineNativeObject(TArray<TArray<uint32_t>>, BlockLinks, AFortNavigationGraph_BlockLinks)
    DefineNativeObject(TArray<int32_t>, DirtyBlocks, AFortNavigationGraph_DirtyBlocks)
    DefineNativeMember(uint32_t, GraphFlags, AFortNavigationGraph_Flags)
    DefineNativeMember(void*, PathSearch, AFortNavigationGraph_PathSearch)

public:
    static AFortNavigationGraph* GetCurrent(UWorld* World);
    static AFortNavigationGraph* FindOrSpawn(UWorld* World);
    static bool ResolveNatives();

    bool InitializeCells(UBuildingStructuralSupportSystem* SupportSystem);
    void OnWorldInitialized();

private:
    bool AreCellsInitialized() const;
    int32_t LoadTerrainCells(UWorld* World);
    void BuildLinks(TArray<int32_t> Blocks, int32_t Next, double ElapsedMs);
};
