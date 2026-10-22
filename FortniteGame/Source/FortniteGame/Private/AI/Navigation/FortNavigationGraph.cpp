#include "pch.h"
#include "AI/Navigation/FortNavigationGraph.h"
#include "AI/FortNavSystem.h"
#include "Building/BuildingStructuralSupportSystem.h"
#include "Engine/Level.h"
#include "Engine/LevelStreaming.h"
#include "Engine/World.h"
#include "Finder.h"
#include "FortWorldSettings.h"
#include "GameThread.h"
#include "HAL/UnrealMemory.h"
#include "Log.h"
#include "Logging/LogMacros.h"
#include "Math/Transform.h"
#include "Templates/Casts.h"
#include "UObject/UObjectIterator.h"
#include "UObject/WeakObjectPtr.h"

#include <chrono>
#include <cmath>
#include <initializer_list>

DEFINE_LOG_CATEGORY_STATIC(LogNavigation, Log, All);

namespace
{
    constexpr int32_t MaxGridCells = 256;
    constexpr int32_t BlockShift[3] = { 3, 3, 2 };
    constexpr uint32_t SuspendBlockUpdates = 1;
    constexpr int32_t BlocksPerTick = 64;
    constexpr size_t PathSearchSize = 0x90;
    constexpr int32_t WorldCellsHeader = 0x7FFFFFFF;
    constexpr int32_t WorldCellsVersion = 1;
    constexpr float CellSizeXY = 512.0f;
    constexpr float CellSizeZ = 384.0f;

    uint8_t Byte(uintptr_t Address)
    {
        return *reinterpret_cast<const uint8_t*>(Address);
    }

    int32_t Displacement(uintptr_t Address)
    {
        return *reinterpret_cast<const int32_t*>(Address);
    }

    uintptr_t FindPattern(uintptr_t Begin, uintptr_t End, std::initializer_list<int32_t> Pattern)
    {
        const size_t Length = Pattern.size();
        for (uintptr_t Address = Begin; Address + Length <= End; ++Address)
        {
            size_t Index = 0;
            for (int32_t Expected : Pattern)
            {
                if (Expected >= 0 && Byte(Address + Index) != static_cast<uint8_t>(Expected))
                {
                    break;
                }
                ++Index;
            }
            if (Index == Length)
            {
                return Address;
            }
        }
        return 0;
    }

    uintptr_t FindUnique(std::initializer_list<int32_t> Pattern)
    {
        uintptr_t Found = 0;
        int32_t Matches = 0;
        for (uintptr_t Site = FindPattern(Finder::TextBegin(), Finder::TextEnd(), Pattern); Site; Site = FindPattern(Site + 1, Finder::TextEnd(), Pattern))
        {
            Found = Site;
            ++Matches;
        }
        return Matches == 1 ? Found : 0;
    }

    uintptr_t FirstCallTarget(uintptr_t Function, size_t MaxDistance)
    {
        for (uintptr_t Site = Function; Function && Site + 5 <= Function + MaxDistance; ++Site)
        {
            if (Byte(Site) == 0xE8)
            {
                const uintptr_t Target = Finder::RelativeTarget(Site, 1, 5);
                if (Target >= Finder::TextBegin() && Target < Finder::TextEnd())
                {
                    return Target;
                }
            }
        }
        return 0;
    }

    double NowMs()
    {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    uint8_t RotateDirections(uint8_t Directions, int32_t Quarter)
    {
        const int32_t Shift = (Quarter & 3) * 2;
        return Shift ? static_cast<uint8_t>((Directions << Shift) | (Directions >> (8 - Shift))) : Directions;
    }

    float TransformYaw(const FTransform& Transform)
    {
        const FQuat& Q = Transform.Rotation;
        return std::atan2(2.0f * (Q.W * Q.Z + Q.X * Q.Y), 1.0f - 2.0f * (Q.Y * Q.Y + Q.Z * Q.Z)) * (180.0f / 3.1415926535897932f);
    }

    void RotateQuarter(float X, float Y, int32_t Quarter, float& OutX, float& OutY)
    {
        switch (Quarter & 3)
        {
        case 1: OutX = -Y; OutY = X; break;
        case 2: OutX = -X; OutY = -Y; break;
        case 3: OutX = Y; OutY = -X; break;
        default: OutX = X; OutY = Y; break;
        }
    }

    bool IsAligned(float Value, float Step)
    {
        return std::fabs(Value - std::round(Value / Step) * Step) <= 1.0f;
    }
}

bool AFortNavigationGraph::ResolveNatives()
{
    if (Natives::AFortNavigationGraph_UpdateDirtyBlocks)
    {
        return true;
    }

    const uintptr_t GridOriginZ = FindUnique({ 0xC7, 0x81, -1, -1, 0x00, 0x00, 0x00, 0x00, 0x70, 0xC5 });
    const uintptr_t InitializeGrid = GridOriginZ ? Finder::FunctionStart(GridOriginZ) : 0;
    const int32_t GridOrigin = GridOriginZ ? Displacement(GridOriginZ + 2) - 8 : -1;
    const uintptr_t GridOriginStore = InitializeGrid ? FindPattern(InitializeGrid, GridOriginZ, { 0xF2, 0x0F, 0x11, 0x81, GridOrigin & 0xFF, (GridOrigin >> 8) & 0xFF, 0x00, 0x00 }) : 0;

    const uintptr_t UpdateDirtyBlocks = FindUnique({ 0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x33, 0xFF, 0x48, 0xC7, 0x81, -1, -1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xD9, 0x39, 0xB9 });
    const uintptr_t UpdateDirtyBlocksEnd = UpdateDirtyBlocks ? Finder::FunctionEnd(UpdateDirtyBlocks) : 0;
    const int32_t DirtyBlocks = UpdateDirtyBlocks ? Displacement(UpdateDirtyBlocks + 28) - 8 : -1;
    const uintptr_t ClearDirtyFlag = UpdateDirtyBlocksEnd ? FindPattern(UpdateDirtyBlocks, UpdateDirtyBlocksEnd, { 0x83, 0xA3, -1, -1, 0x00, 0x00, 0xFD }) : 0;
    const uintptr_t BuildBlockLinks = UpdateDirtyBlocks ? FirstCallTarget(UpdateDirtyBlocks, 0x60) : 0;
    const uintptr_t BlockLinksAdd = BuildBlockLinks ? FindPattern(BuildBlockLinks, BuildBlockLinks + 0x80, { 0x49, 0x03, 0x8D, -1, -1, 0x00, 0x00 }) : 0;
    const uintptr_t BlockCellsLoad = BuildBlockLinks ? FindPattern(BuildBlockLinks, BuildBlockLinks + 0x100, { 0x49, 0x8B, 0x85, -1, -1, 0x00, 0x00 }) : 0;

    const uintptr_t CellIndexBody = FindUnique({ 0x8B, 0x52, 0x08, 0x8B, 0x49, 0x38, 0xD3, 0xFA, 0x0F, 0xAF, 0x53, 0x1C });
    const uintptr_t PathSearchBody = FindUnique({ 0x48, 0x8D, 0x59, 0x08, 0x48, 0x89, 0x11, 0x33, 0xF6, 0x48, 0x8B, 0xF9, 0x48, 0x89, 0x33 });
    const uintptr_t FloodFill = FindUnique({ 0x40, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x48, 0x48, 0x8B, 0xF9, 0x4D, 0x8B, 0xF0, 0x48, 0x8B, 0x89 });
    const std::vector<uintptr_t> QueueFull = Finder::StringRefs(L"Navigation System: registration queue full!");
    const uintptr_t RequestRegistration = QueueFull.empty() ? 0 : Finder::FunctionStart(QueueFull.front());

    if (!InitializeGrid || !GridOriginStore || !UpdateDirtyBlocksEnd || !ClearDirtyFlag || !BlockLinksAdd || !BlockCellsLoad || !CellIndexBody || !PathSearchBody || !FloodFill || !RequestRegistration)
    {
        Log::Error(L"Finder: AFortNavigationGraph natives not found (grid {}, origin {}, dirty {}, flags {}, links {}, cells {}, cell index {}, path search {}, flood fill {}, registration {})",
            InitializeGrid != 0, GridOriginStore != 0, UpdateDirtyBlocksEnd != 0, ClearDirtyFlag != 0, BlockLinksAdd != 0, BlockCellsLoad != 0, CellIndexBody != 0, PathSearchBody != 0, FloodFill != 0, RequestRegistration != 0);
        return false;
    }

    Natives::UBuildingStructuralSupportSystem_InitializeGrid = reinterpret_cast<decltype(Natives::UBuildingStructuralSupportSystem_InitializeGrid)>(InitializeGrid);
    Natives::UBuildingStructuralSupportSystem_GridOrigin = GridOrigin;
    Natives::UBuildingStructuralSupportSystem_GridSize = GridOrigin - 12;
    Natives::AFortNavigationGraph_DirtyBlocks = DirtyBlocks;
    Natives::AFortNavigationGraph_Flags = Displacement(ClearDirtyFlag + 2);
    Natives::AFortNavigationGraph_BlockLinks = Displacement(BlockLinksAdd + 3);
    Natives::AFortNavigationGraph_Cells = Displacement(BlockCellsLoad + 3) - static_cast<int32_t>(offsetof(FNavGraphCells, BlockCells));
    Natives::AFortNavigationGraph_PathSearch = Displacement(FloodFill + 17);
    Natives::AFortNavigationGraph_UpdateDirtyBlocks = reinterpret_cast<decltype(Natives::AFortNavigationGraph_UpdateDirtyBlocks)>(UpdateDirtyBlocks);
    Natives::FNavGraphCells_GetCellIndex = reinterpret_cast<decltype(Natives::FNavGraphCells_GetCellIndex)>(Finder::FunctionStart(CellIndexBody));
    Natives::FNavGraphPathSearch_Construct = reinterpret_cast<decltype(Natives::FNavGraphPathSearch_Construct)>(Finder::FunctionStart(PathSearchBody));
    Natives::UNavigationSystem_RequestRegistration = reinterpret_cast<decltype(Natives::UNavigationSystem_RequestRegistration)>(RequestRegistration);
    Log::Info(L"Finder: AFortNavigationGraph cells +0x{:X}, block links +0x{:X}, dirty blocks +0x{:X}, flags +0x{:X}, path search +0x{:X}; support grid size +0x{:X}, origin +0x{:X}",
        Natives::AFortNavigationGraph_Cells, Natives::AFortNavigationGraph_BlockLinks, Natives::AFortNavigationGraph_DirtyBlocks, Natives::AFortNavigationGraph_Flags, Natives::AFortNavigationGraph_PathSearch,
        Natives::UBuildingStructuralSupportSystem_GridSize, Natives::UBuildingStructuralSupportSystem_GridOrigin);
    return true;
}

AFortNavigationGraph* AFortNavigationGraph::GetCurrent(UWorld* World)
{
    if (!World)
    {
        return nullptr;
    }
    if (UFortNavSystem* NavSys = Cast<UFortNavSystem>(reinterpret_cast<UObject*>(World->GetNavigationSystem())))
    {
        if (AFortNavigationGraph* Graph = NavSys->NavGraphData)
        {
            return Graph;
        }
    }
    for (TObjectIterator<AFortNavigationGraph> It; It; ++It)
    {
        if (It->GetWorld() == World)
        {
            return *It;
        }
    }
    return nullptr;
}

AFortNavigationGraph* AFortNavigationGraph::FindOrSpawn(UWorld* World)
{
#if WITH_SERVER_CODE
    if (AFortNavigationGraph* Graph = GetCurrent(World))
    {
        return Graph;
    }

    UNavigationSystem* NavSys = World ? World->GetNavigationSystem() : nullptr;
    if (!NavSys)
    {
        return nullptr;
    }

    AFortNavigationGraph* Graph = World->SpawnActor<AFortNavigationGraph>(StaticClass());
    if (!Graph)
    {
        return nullptr;
    }

    TStructArray<FNavDataConfig>& SupportedAgents = NavSys->SupportedAgents;
    for (int32_t Index = 0; Index < SupportedAgents.Num(); ++Index)
    {
        FNavDataConfig& Agent = SupportedAgents[Index];
        if (Agent.NavigationDataClass == StaticClass())
        {
            FNavDataConfig::StaticStruct()->CopyScriptStruct(&Graph->NavDataConfig, &Agent);
            NavSys->RequestRegistration(Graph, true);
            break;
        }
    }
    return Graph;
#else
    return nullptr;
#endif
}

bool AFortNavigationGraph::AreCellsInitialized() const
{
    return Natives::AFortNavigationGraph_Cells >= 0 && Cells.NumBlocks > 0 && Cells.BlockCells.Num() == Cells.NumBlocks;
}

bool AFortNavigationGraph::InitializeCells(UBuildingStructuralSupportSystem* SupportSystem)
{
#if WITH_SERVER_CODE
    FVector GridOrigin;
    int32_t GridSize[3] = {};
    if (!SupportSystem || !Natives::FNavGraphPathSearch_Construct || !SupportSystem->GetGrid(GridOrigin, GridSize))
    {
        return false;
    }

    FNavGraphCells& Grid = Cells;
    if (!AreCellsInitialized())
    {
        const float Origin[3] = { GridOrigin.X / CellSizeXY, GridOrigin.Y / CellSizeXY, GridOrigin.Z / CellSizeZ };
        int32_t Size[3] = {};
        for (int32_t Axis = 0; Axis < 3; ++Axis)
        {
            Grid.Origin[Axis] = static_cast<int32_t>(std::lround(Origin[Axis]));
            Size[Axis] = GridSize[Axis] < MaxGridCells ? GridSize[Axis] : MaxGridCells;
        }
        if (Size[0] != GridSize[0] || Size[1] != GridSize[1] || Size[2] != GridSize[2])
        {
            UE_LOG(LogNavigation, Error, TEXT("Levels bounds are too large! Current(x:%d y:%d z:%d) Max(x:%d y:%d z:%d). Make sure that all FortStaticMeshActors are placed in tile/foundation boundaries."),
                GridSize[0], GridSize[1], GridSize[2], MaxGridCells, MaxGridCells, MaxGridCells);
        }

        Grid.NumBlocks = 1;
        for (int32_t Axis = 0; Axis < 3; ++Axis)
        {
            Grid.Size[Axis] = Size[Axis];
            Grid.BlockShift[Axis] = BlockShift[Axis];
            Grid.BlockMask[Axis] = (1 << BlockShift[Axis]) - 1;
            Grid.Blocks[Axis] = (Size[Axis] + Grid.BlockMask[Axis]) >> BlockShift[Axis];
            Grid.NumBlocks *= Grid.Blocks[Axis];
        }
        Grid.CellsPerBlock = 1 << (BlockShift[0] + BlockShift[1] + BlockShift[2]);

        Grid.BlockCells.Reset();
        Grid.BlockCells.AddUninitialized(Grid.NumBlocks);
        for (int32_t& BlockCell : Grid.BlockCells)
        {
            BlockCell = -1;
        }
        Grid.Cells.Reset();

        TArray<TArray<uint32_t>>& Links = BlockLinks;
        Links.Reset();
        Links.AddZeroed(Grid.NumBlocks);
    }

    if (!PathSearch)
    {
        PathSearch = Natives::FNavGraphPathSearch_Construct(FMemory::Malloc(PathSearchSize), this);
    }

    GraphFlags = GraphFlags | SuspendBlockUpdates;
    SupportSystem->NavGraph = this;
    Log::Info(L"AFortNavigationGraph: cells origin ({}, {}, {}) size {}x{}x{}, {} blocks of {} cells, linked to {}",
        Grid.Origin[0], Grid.Origin[1], Grid.Origin[2], Grid.Size[0], Grid.Size[1], Grid.Size[2], Grid.NumBlocks, Grid.CellsPerBlock, SupportSystem->GetName().ToStdString());
    return true;
#else
    return false;
#endif
}

int32_t AFortNavigationGraph::LoadTerrainCells(UWorld* World)
{
    int32_t Loaded = 0;
    for (TObjectIterator<AFortWorldSettings> It; It; ++It)
    {
        AFortWorldSettings* Settings = *It;
        if (Settings->GetWorld() != World || Settings->HasAnyFlags(RF_ArchetypeObject))
        {
            continue;
        }

        const TArray<int32_t>& WorldCells = Settings->WorldCells;
        ULevel* Level = Cast<ULevel>(Settings->GetOuter());
        if (WorldCells.Num() < 2 || WorldCells[0] != WorldCellsHeader || WorldCells[1] != WorldCellsVersion || !Level)
        {
            continue;
        }

        FTransform LevelTransform;
        for (TObjectIterator<ULevelStreaming> Streaming; Streaming; ++Streaming)
        {
            if (Streaming->LoadedLevel == Level)
            {
                LevelTransform = Streaming->LevelTransform;
                break;
            }
        }

        const FVector& Origin = Settings->WorldCellsOrigin;
        const float Yaw = TransformYaw(LevelTransform);
        const int32_t Quarter = static_cast<int32_t>(((std::lround(Yaw / 90.0f) % 4) + 4) % 4);
        float OriginX = 0.0f;
        float OriginY = 0.0f;
        RotateQuarter(Origin.X, Origin.Y, Quarter, OriginX, OriginY);
        OriginX += LevelTransform.Translation.X;
        OriginY += LevelTransform.Translation.Y;
        const float OriginZ = Origin.Z + LevelTransform.Translation.Z;
        if (std::fabs(Yaw - std::round(Yaw / 90.0f) * 90.0f) > 1.0f || !IsAligned(OriginX, CellSizeXY) || !IsAligned(OriginY, CellSizeXY) || !IsAligned(OriginZ, CellSizeZ))
        {
            UE_LOG(LogNavigation, Warning, TEXT("Misaligned terrain data found in level [%s]!"), *Level->GetOutermost()->GetName());
            continue;
        }

        FNavGraphCells& Grid = Cells;
        const int32_t BaseX = static_cast<int32_t>(std::lround(OriginX / CellSizeXY)) - Grid.Origin[0];
        const int32_t BaseY = static_cast<int32_t>(std::lround(OriginY / CellSizeXY)) - Grid.Origin[1];
        const int32_t BaseZ = static_cast<int32_t>(std::lround(OriginZ / CellSizeZ)) - Grid.Origin[2];
        int32_t MaxX = 0;
        int32_t MaxY = 0;
        bool bOutside = false;
        for (int32_t Index = 2; Index + 3 < WorldCells.Num(); Index += 4)
        {
            const uint32_t Location = static_cast<uint32_t>(WorldCells[Index]);
            const uint32_t Links = static_cast<uint32_t>(WorldCells[Index + 1]);
            const uint32_t Steps = static_cast<uint32_t>(WorldCells[Index + 2]);
            const uint8_t GroundLinks = static_cast<uint8_t>(Links >> 12);
            const uint8_t StepsUp = static_cast<uint8_t>(Steps);
            const uint8_t StepsDown = static_cast<uint8_t>(Steps >> 8);
            if (!GroundLinks && !StepsUp && !StepsDown)
            {
                continue;
            }

            float CellX = 0.0f;
            float CellY = 0.0f;
            RotateQuarter(static_cast<float>(Location & 0xFFF), static_cast<float>((Location >> 12) & 0xFFF), Quarter, CellX, CellY);
            const int32_t Coords[3] = { BaseX + static_cast<int32_t>(CellX), BaseY + static_cast<int32_t>(CellY), BaseZ + static_cast<int32_t>(Links & 0xFFF) };
            if (Coords[0] < 0 || Coords[0] >= Grid.Size[0] || Coords[1] < 0 || Coords[1] >= Grid.Size[1] || Coords[2] < 0 || Coords[2] >= Grid.Size[2])
            {
                bOutside = true;
                MaxX = Coords[0] > MaxX ? Coords[0] : MaxX;
                MaxY = Coords[1] > MaxY ? Coords[1] : MaxY;
                continue;
            }

            const int32_t CellIndex = Natives::FNavGraphCells_GetCellIndex(&Grid, Coords);
            if (CellIndex < 0 || CellIndex >= Grid.Cells.Num())
            {
                continue;
            }
            FNavGraphCell& Cell = Grid.Cells[CellIndex];
            Cell.GroundLinks |= RotateDirections(GroundLinks, Quarter);
            Cell.GroundSteps |= static_cast<uint16_t>((RotateDirections(StepsUp, Quarter) << 8) | RotateDirections(StepsDown, Quarter));
            ++Loaded;
        }

        if (bOutside)
        {
            UE_LOG(LogNavigation, Warning, TEXT("Terrain in level:%s is larger than building system bounds! NewMaxX:%d NewMaxY:%d"), *Level->GetOutermost()->GetName(), MaxX, MaxY);
        }
    }
    return Loaded;
}

void AFortNavigationGraph::OnWorldInitialized()
{
#if WITH_SERVER_CODE
    UWorld* World = GetWorld();
    if (!World || !AreCellsInitialized() || !Natives::AFortNavigationGraph_UpdateDirtyBlocks)
    {
        Log::Warning(L"AFortNavigationGraph: no cells to build (support grid not initialized)");
        return;
    }

    UFortNavSystem* NavSys = Cast<UFortNavSystem>(reinterpret_cast<UObject*>(World->GetNavigationSystem()));
    if (NavSys && NavSys->NavGraphData != this)
    {
        NavSys->OnNavDataRegistered(this);
    }

    const double Started = NowMs();
    const int32_t TerrainCells = LoadTerrainCells(World);
    const FNavGraphCells& Grid = Cells;
    TArray<int32_t> Blocks;
    for (int32_t Block = 0; Block < Grid.BlockCells.Num(); ++Block)
    {
        if (Grid.BlockCells[Block] != -1)
        {
            Blocks.Add(Block);
        }
    }
    Log::Info(L"AFortNavigationGraph: {} terrain cells, {} cells in {} blocks", TerrainCells, Grid.Cells.Num(), Blocks.Num());
    BuildLinks(Blocks, 0, NowMs() - Started);
#endif
}

void AFortNavigationGraph::BuildLinks(TArray<int32_t> Blocks, int32_t Next, double ElapsedMs)
{
    const double Started = NowMs();
    TArray<int32_t>& Dirty = DirtyBlocks;
    const int32_t End = Next + BlocksPerTick < Blocks.Num() ? Next + BlocksPerTick : Blocks.Num();
    for (int32_t Index = Next; Index < End; ++Index)
    {
        Dirty.Add(Blocks[Index]);
    }
    Natives::AFortNavigationGraph_UpdateDirtyBlocks(this);
    ElapsedMs += NowMs() - Started;

    if (End < Blocks.Num())
    {
        FWeakObjectPtr WeakGraph(reinterpret_cast<UObject*>(this));
        GameThread::Enqueue([WeakGraph, Blocks, End, ElapsedMs]()
        {
            if (AFortNavigationGraph* Graph = static_cast<AFortNavigationGraph*>(WeakGraph.Get()))
            {
                Graph->BuildLinks(Blocks, End, ElapsedMs);
            }
        });
        return;
    }

    GraphFlags = GraphFlags & ~SuspendBlockUpdates;

    const FNavGraphCells& Grid = Cells;
    const TArray<TArray<uint32_t>>& Links = BlockLinks;
    size_t Bytes = static_cast<size_t>(Grid.Cells.Max()) * sizeof(FNavGraphCell) + static_cast<size_t>(Grid.BlockCells.Max()) * sizeof(int32_t) + static_cast<size_t>(Links.Max()) * sizeof(TArray<uint32_t>);
    for (const TArray<uint32_t>& BlockLinkList : Links)
    {
        Bytes += static_cast<size_t>(BlockLinkList.Max()) * sizeof(uint32_t);
    }
    UE_LOG(LogNavigation, Log, TEXT("AFortNavigationGraph::OnWorldInitialized time:%.3fms memory:%.1fkB"), ElapsedMs, static_cast<double>(Bytes) / 1024.0);
}
