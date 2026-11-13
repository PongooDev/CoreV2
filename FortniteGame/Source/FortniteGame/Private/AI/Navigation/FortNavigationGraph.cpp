#include "pch.h"
#include "AI/Navigation/FortNavigationGraph.h"
#include "AI/FortNavSystem.h"
#include "Building/BuildingStructuralSupportSystem.h"
#include "Engine/Level.h"
#include "Engine/LevelStreaming.h"
#include "Engine/World.h"
#include "Finder.h"
#include "FortGameState.h"
#include "FortWorldSettings.h"
#include "HAL/UnrealMemory.h"
#include "Log.h"
#include "Logging/LogMacros.h"
#include "Math/Transform.h"
#include "Templates/Casts.h"
#include "UObject/UObjectIterator.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <initializer_list>

DEFINE_LOG_CATEGORY_STATIC(LogNavigation, Log, All);

namespace
{
    constexpr int32_t MaxGridCells = 256;
    constexpr int32_t BlockShift[3] = { 3, 3, 2 };
    constexpr uint32_t SuspendBlockUpdates = 1;
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

    // The disp32 of `Opcode Reg, [this+disp32]` where BuildBlockLinks keeps `this` in r13 (1.1, 1.2) or r12 (1.7.2,
    // whose base takes a SIB byte).
    uintptr_t ThisDisplacement(uintptr_t Begin, uintptr_t End, uint8_t Opcode, uint8_t Reg)
    {
        for (uintptr_t Site = Begin; Site + 8 <= End; ++Site)
        {
            if (Byte(Site) != 0x49 || Byte(Site + 1) != Opcode)
            {
                continue;
            }
            if (Byte(Site + 2) == (0x80 | (Reg << 3) | 5) && Byte(Site + 5) == 0x00 && Byte(Site + 6) == 0x00)
            {
                return Site + 3;
            }
            if (Byte(Site + 2) == (0x80 | (Reg << 3) | 4) && Byte(Site + 3) == 0x24 && Byte(Site + 6) == 0x00 && Byte(Site + 7) == 0x00)
            {
                return Site + 4;
            }
        }
        return 0;
    }

    // The same for 5.0+, whose BuildBlockLinks keeps `this` in rsi: `48 Opcode modrm(rsi) disp32`.
    uintptr_t RsiDisplacement(uintptr_t Begin, uintptr_t End, uint8_t Opcode, uint8_t Reg)
    {
        for (uintptr_t Site = Begin; Site + 7 <= End; ++Site)
        {
            if (Byte(Site) == 0x48 && Byte(Site + 1) == Opcode && Byte(Site + 2) == (0x80 | (Reg << 3) | 6) && Byte(Site + 5) == 0x00 && Byte(Site + 6) == 0x00)
            {
                return Site + 3;
            }
        }
        return 0;
    }

    // FGraphAStar<FNavGraphCells>'s constructor (FNavGraphPathSearch, 0x90 bytes), which 5.0+ inlines into its users (5.30
    // 0xAEE2EB): Graph; NodePool, a TSet of search nodes (Elements' FirstFreeIndex -1); OpenList, which refers to NodePool and
    // sorts with a predicate that refers to it too. 1.1-4.5 call the game's own copy (4.5 0x957CB0); its Reserve(64) calls
    // only presize the node pool and the open list, which grow on first use without them.
    void* ConstructNavGraphPathSearch(void* Memory, AFortNavigationGraph* Graph)
    {
        if (!Memory)
        {
            return nullptr;
        }
        uint8_t* Search = static_cast<uint8_t*>(Memory);
        std::memset(Search, 0, PathSearchSize);
        uint8_t* NodePool = Search + 0x08;
        *reinterpret_cast<AFortNavigationGraph**>(Search) = Graph;
        *reinterpret_cast<int32_t*>(NodePool + 0x40) = -1;
        *reinterpret_cast<uint8_t**>(Search + 0x68) = NodePool;
        *reinterpret_cast<uint8_t**>(Search + 0x80) = NodePool;
        *reinterpret_cast<uint8_t**>(Search + 0x88) = Search + 0x68;
        return Memory;
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
    uintptr_t InitializeGrid = GridOriginZ ? Finder::FunctionStart(GridOriginZ) : 0;
    int32_t GridOrigin = GridOriginZ ? Displacement(GridOriginZ + 2) - 8 : -1;
    uintptr_t GridOriginStore = InitializeGrid ? FindPattern(InitializeGrid, GridOriginZ, { 0xF2, 0x0F, 0x11, 0x81, GridOrigin & 0xFF, (GridOrigin >> 8) & 0xFF, 0x00, 0x00 }) : 0;
    if (!GridOriginZ)
    {
        // 5.0+ (UE 4.21) has no hard-coded -3840 Z: InitializeGrid takes the grid's bottom cell (Origin.Z = 384 * MinCellZ) as a
        // seventh argument. Its opening stores the grid center: or [this+flags], 1; movsd xmm0, [rdx];
        // movsd [this+GridOrigin], xmm0; mov eax, [rdx+8] (5.30 0xD259FD, 5.21 0xD07C9D, 5.0 0xCE2E1D).
        const uintptr_t CenterStore = FindUnique({ 0x83, 0x89, -1, -1, 0x00, 0x00, 0x01, 0xF2, 0x0F, 0x10, 0x02, 0xF2, 0x0F, 0x11, 0x81, -1, -1, 0x00, 0x00, 0x8B, 0x42, 0x08 });
        InitializeGrid = CenterStore ? Finder::FunctionStart(CenterStore) : 0;
        GridOrigin = CenterStore ? Displacement(CenterStore + 15) : -1;
        GridOriginStore = InitializeGrid ? CenterStore + 11 : 0;
    }

    // mov [rsp+10h], rbx; push rdi; sub rsp, 20h on 1.1-4.5; 5.0+ saves four registers first, so only the body after the
    // prologue is the same: xor edi, edi; mov qword [rcx+DirtyBlocks+18h], 0; mov rbx, rcx; cmp [rcx+DirtyBlocks+8], edi.
    const std::initializer_list<int32_t> UpdateDirtyBlocksBody = { 0x33, 0xFF, 0x48, 0xC7, 0x81, -1, -1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xD9, 0x39, 0xB9 };
    uintptr_t UpdateDirtyBlocks = FindUnique({ 0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x33, 0xFF, 0x48, 0xC7, 0x81, -1, -1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xD9, 0x39, 0xB9 });
    uintptr_t UpdateDirtyBlocksBodyStart = UpdateDirtyBlocks ? UpdateDirtyBlocks + 10 : 0;
    if (!UpdateDirtyBlocks)
    {
        UpdateDirtyBlocksBodyStart = FindUnique(UpdateDirtyBlocksBody);
        UpdateDirtyBlocks = UpdateDirtyBlocksBodyStart ? Finder::FunctionStart(UpdateDirtyBlocksBodyStart) : 0;
    }
    const uintptr_t UpdateDirtyBlocksEnd = UpdateDirtyBlocks ? Finder::FunctionEnd(UpdateDirtyBlocks) : 0;
    const int32_t DirtyBlocks = UpdateDirtyBlocks ? Displacement(UpdateDirtyBlocksBodyStart + 18) - 8 : -1;
    const uintptr_t ClearDirtyFlag = UpdateDirtyBlocksEnd ? FindPattern(UpdateDirtyBlocks, UpdateDirtyBlocksEnd, { 0x83, 0xA3, -1, -1, 0x00, 0x00, 0xFD }) : 0;
    const uintptr_t BuildBlockLinks = UpdateDirtyBlocks ? FirstCallTarget(UpdateDirtyBlocks, 0x60) : 0;
    // add rcx, [this+BlockLinks]; mov rax, [this+Cells.BlockCells]. 5.0+ keeps `this` in rsi (5.30 0xAF5FD0, 0xAF5FF1).
    uintptr_t BlockLinksAdd = BuildBlockLinks ? ThisDisplacement(BuildBlockLinks, BuildBlockLinks + 0x80, 0x03, 1) : 0;
    uintptr_t BlockCellsLoad = BuildBlockLinks ? ThisDisplacement(BuildBlockLinks, BuildBlockLinks + 0x100, 0x8B, 0) : 0;
    if (BuildBlockLinks && (!BlockLinksAdd || !BlockCellsLoad))
    {
        BlockLinksAdd = RsiDisplacement(BuildBlockLinks, BuildBlockLinks + 0x80, 0x03, 1);
        BlockCellsLoad = RsiDisplacement(BuildBlockLinks, BuildBlockLinks + 0x100, 0x8B, 0);
    }

    uintptr_t CellIndexBody = FindUnique({ 0x8B, 0x52, 0x08, 0x8B, 0x49, 0x38, 0xD3, 0xFA, 0x0F, 0xAF, 0x53, 0x1C });
    if (!CellIndexBody)
    {
        // 5.0+ loads the coordinates into other registers and allocates a missing block inline: mov ecx, [rcx+38h] (BlockShift
        // Z) ... sar r8d, cl; imul r8d, [rsi+1Ch] (Blocks Y) (5.30 0xAE7CE0).
        CellIndexBody = FindUnique({ 0x8B, 0x49, 0x38, -1, -1, -1, -1, -1, -1, -1, -1, 0xD3, -1, -1, 0x0F, 0xAF, -1, 0x1C });
    }
    const std::initializer_list<int32_t> PathSearchPattern = { 0x48, 0x8D, 0x59, 0x08, 0x48, 0x89, 0x11, 0x33, 0xF6, 0x48, 0x8B, 0xF9, 0x48, 0x89, 0x33 };
    uintptr_t PathSearchBody = FindUnique(PathSearchPattern);
    if (!PathSearchBody)
    {
        // 3.5.1 has a second constructor of the same shape (0x90E390, the flood fill's search of 32-byte nodes): take the
        // one whose first call reserves FNavGraphPathSearch's 40-byte nodes (0x90E430: lea rcx, [rbx+rbx*4]; xor edx,
        // edx; shl rcx, 3 in 0xDBEA50).
        int32_t Matches = 0;
        for (uintptr_t Site = FindPattern(Finder::TextBegin(), Finder::TextEnd(), PathSearchPattern); Site; Site = FindPattern(Site + 1, Finder::TextEnd(), PathSearchPattern))
        {
            const uintptr_t Reserve = FirstCallTarget(Site, 0x50);
            if (Reserve && FindPattern(Reserve, Reserve + 0x20, { 0x48, 0x8D, 0x0C, 0x9B, 0x33, 0xD2, 0x48, 0xC1, 0xE1, 0x03 }))
            {
                PathSearchBody = Site;
                ++Matches;
            }
        }
        PathSearchBody = Matches == 1 ? PathSearchBody : 0;
    }
    bool bConstructPathSearch = false;
    if (!PathSearchBody)
    {
        // 5.0+ inlines the constructor into its users (5.30 0xAEE2EB, 0xAF48C3): FGraphAStar's same 0x90 bytes, built in place
        // by Core's FNavGraphPathSearch constructor. Take it only when the inlined copy shows the layout:
        // mov dword [rbx+40h], -1 (NodePool's FirstFreeIndex) ... cmp dword [rbx+0Ch], 40h ... mov [rsi+68h], rbx (OpenList).
        const std::initializer_list<int32_t> InlinedLayout = { 0xC7, 0x43, 0x40, 0xFF, 0xFF, 0xFF, 0xFF };
        for (uintptr_t Site = FindPattern(Finder::TextBegin(), Finder::TextEnd(), InlinedLayout); Site && !bConstructPathSearch; Site = FindPattern(Site + 1, Finder::TextEnd(), InlinedLayout))
        {
            const uintptr_t Reserve = FindPattern(Site, Site + 0x20, { 0x83, 0x7B, 0x0C, 0x40 });
            bConstructPathSearch = Reserve && FindPattern(Reserve, Reserve + 0x20, { 0x48, 0x89, -1, 0x68 }) && PathSearchSize == 0x90;
        }
    }
    uintptr_t FloodFill = FindUnique({ 0x40, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x48, 0x48, 0x8B, 0xF9, 0x4D, 0x8B, 0xF0, 0x48, 0x8B, 0x89 });
    uintptr_t PathSearchLoad = FloodFill ? FloodFill + 17 : 0;
    for (const uintptr_t Ref : FloodFill ? std::vector<uintptr_t>() : Finder::StringRefs(L"STAT_NavGraphFlowField"))
    {
        // 3.5.1's (0x920050) opens with SCOPE_CYCLE_COUNTER(STAT_NavGraphFlowField), as the function after it does: the
        // flood fill keeps this in rdi and loads the path search into rcx, mov rdi, rcx; mov rcx, [rcx+618h].
        const uintptr_t Function = Finder::FunctionStart(Ref);
        const uintptr_t Load = Function ? FindPattern(Function, Function + 0x40, { 0x48, 0x8B, 0xF9, 0x48, 0x8B, 0x89, -1, -1, 0x00, 0x00 }) : 0;
        if (Load && !FloodFill)
        {
            FloodFill = Function;
            PathSearchLoad = Load + 6;
        }
    }
    // Two functions log the full queue; RequestRegistration(this, NavData, bTriggerRegistrationProcessing) is the one that
    // spills NavData (mov [rsp+10h], rdx) in its prologue, the other registers every nav data actor in the world.
    uintptr_t RequestRegistration = 0;
    for (const uintptr_t Ref : Finder::StringRefs(L"Navigation System: registration queue full!"))
    {
        const uintptr_t Function = Finder::FunctionStart(Ref);
        if (Function && !RequestRegistration && FindPattern(Function, Function + 0x10, { 0x48, 0x89, 0x54, 0x24, 0x10 }))
        {
            RequestRegistration = Function;
        }
    }

    if (!InitializeGrid || !GridOriginStore || !UpdateDirtyBlocksEnd || !ClearDirtyFlag || !BlockLinksAdd || !BlockCellsLoad || !CellIndexBody || (!PathSearchBody && !bConstructPathSearch) || !FloodFill || !RequestRegistration)
    {
        Log::Error(L"Finder: AFortNavigationGraph natives not found (grid {}, origin {}, dirty {}, flags {}, links {}, cells {}, cell index {}, path search {}, flood fill {}, registration {})",
            InitializeGrid != 0, GridOriginStore != 0, UpdateDirtyBlocksEnd != 0, ClearDirtyFlag != 0, BlockLinksAdd != 0, BlockCellsLoad != 0, CellIndexBody != 0, PathSearchBody != 0 || bConstructPathSearch, FloodFill != 0, RequestRegistration != 0);
        return false;
    }

    Natives::UBuildingStructuralSupportSystem_InitializeGrid = reinterpret_cast<decltype(Natives::UBuildingStructuralSupportSystem_InitializeGrid)>(InitializeGrid);
    Natives::UBuildingStructuralSupportSystem_GridOrigin = GridOrigin;
    Natives::UBuildingStructuralSupportSystem_GridSize = GridOrigin - 12;
    Natives::AFortNavigationGraph_DirtyBlocks = DirtyBlocks;
    Natives::AFortNavigationGraph_Flags = Displacement(ClearDirtyFlag + 2);
    Natives::AFortNavigationGraph_BlockLinks = Displacement(BlockLinksAdd);
    Natives::AFortNavigationGraph_Cells = Displacement(BlockCellsLoad) - static_cast<int32_t>(offsetof(FNavGraphCells, BlockCells));
    Natives::AFortNavigationGraph_PathSearch = Displacement(PathSearchLoad);
    Natives::AFortNavigationGraph_UpdateDirtyBlocks = reinterpret_cast<decltype(Natives::AFortNavigationGraph_UpdateDirtyBlocks)>(UpdateDirtyBlocks);
    Natives::FNavGraphCells_GetCellIndex = reinterpret_cast<decltype(Natives::FNavGraphCells_GetCellIndex)>(Finder::FunctionStart(CellIndexBody));
    Natives::FNavGraphPathSearch_Construct = PathSearchBody ? reinterpret_cast<decltype(Natives::FNavGraphPathSearch_Construct)>(Finder::FunctionStart(PathSearchBody)) : &ConstructNavGraphPathSearch;
    Natives::UNavigationSystem_RequestRegistration = reinterpret_cast<decltype(Natives::UNavigationSystem_RequestRegistration)>(RequestRegistration);
    Log::Info(L"Finder: AFortNavigationGraph cells +0x{:X}, block links +0x{:X}, dirty blocks +0x{:X}, flags +0x{:X}, path search +0x{:X}; support grid size +0x{:X}, origin +0x{:X}",
        Natives::AFortNavigationGraph_Cells, Natives::AFortNavigationGraph_BlockLinks, Natives::AFortNavigationGraph_DirtyBlocks, Natives::AFortNavigationGraph_Flags, Natives::AFortNavigationGraph_PathSearch,
        Natives::UBuildingStructuralSupportSystem_GridSize, Natives::UBuildingStructuralSupportSystem_GridOrigin);
    // Looked for with the rest (it needs the cells' offset), so a server's start shows whether the game's loader is used.
    ResolveTerrainLoader();
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

    // The agent the graph is the navigation data of: the zone's FortNavSystem has one (Graph), a Battle Royale world's
    // navigation system has none.
    TStructArray<FNavDataConfig>& SupportedAgents = NavSys->SupportedAgents;
    int32_t GraphAgent = -1;
    for (int32_t Index = 0; Index < SupportedAgents.Num() && GraphAgent < 0; ++Index)
    {
        if (SupportedAgents[Index].NavigationDataClass == StaticClass())
        {
            GraphAgent = Index;
        }
    }

    // No graph: the navigation system hasn't spawned its agents' navigation data. Up to 4.5 it does by itself
    // (bAutoCreateNavigationData); from 5.0 (UE 4.21) a FortNavSystem is made with that off, and Epic's server asks for
    // them in UFortNavSystem::PrepareBoundsAndUnlock (6.00 CL-4395664 0xAF37E0: "if (!NavGraphData)
    // SpawnMissingNavigationData()"). The system's own SpawnMissingNavigationData spawns every supported agent's missing
    // data: FortNavMesh-Husk and FortNavMesh-Smasher, the husks' navmeshes, with the graph. Core needs the graph now,
    // when the support grid is laid out, so the buildings registered from here on fill its cells; the navmeshes come
    // with it, as they are there from world init on the older builds (#408: with only the graph, the husks' agent had no
    // navmesh and nothing could path).
    if (GraphAgent >= 0 && UNavigationSystem::CanSpawnMissingNavigationData())
    {
        const TArray<ANavigationData*>& NavDataSet = NavSys->NavDataSet;
        const int32_t NumNavData = NavDataSet.Num();
        NavSys->SpawnMissingNavigationData();
        NavSys->ProcessRegistrationCandidates();
        std::wstring Names;
        for (ANavigationData* NavData : NavDataSet)
        {
            Names += (Names.empty() ? L"" : L", ") + (NavData ? reinterpret_cast<UObject*>(NavData)->GetName().ToStdString() : std::wstring(L"none"));
        }
        ANavigationData* MainNavData = NavSys->GetDefaultNavDataInstance();
        Log::Info(L"AFortNavigationGraph: the navigation system had no graph, SpawnMissingNavigationData: {} navigation data for {} agents ({} new): {}; main {}", NavDataSet.Num(),
            SupportedAgents.Num(), NavDataSet.Num() - NumNavData, Names, MainNavData ? reinterpret_cast<UObject*>(MainNavData)->GetName().ToStdString() : std::wstring(L"none"));
        if (AFortNavigationGraph* Graph = GetCurrent(World))
        {
            return Graph;
        }
    }

    // The graph by hand, where the system's SpawnMissingNavigationData isn't found or doesn't spawn one.
    AFortNavigationGraph* Graph = World->SpawnActor<AFortNavigationGraph>(StaticClass());
    if (!Graph)
    {
        return nullptr;
    }

    if (GraphAgent >= 0)
    {
        FNavDataConfig::StaticStruct()->CopyScriptStruct(&Graph->NavDataConfig, &SupportedAgents[GraphAgent]);
        NavSys->RequestRegistration(Graph, true);
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

bool AFortNavigationGraph::ResolveTerrainLoader()
{
    static const bool bResolved = []
    {
        // AFortNavigationGraph::OnWorldInitialized (6.02 CL-4461277 0xAD84C0, 6.10 CL-4480234 0xB07860, 6.20 CL-4497486
        // 0xB5FDF0), the function that logs its own time, keeps `this` from its prologue (mov rbx, rcx), lays the cells out
        // over the support grid's bounds (lea rcx, [rbx+Cells]; call, 0xAD20A0) and then hands the terrain to its loader
        // with the world, the support system and itself (mov r8, rbx; mov rdx, rsi; mov rcx, r13; call 0xAD4620; 6.10
        // 0xB038E0, 6.20 0xB5BE70): the function that warns of misaligned terrain. Core lays its cells out earlier (when the
        // support grid is, so the buildings registered from then on fill them), which is why the loader is called on its
        // own; the rest of OnWorldInitialized is Core's (OnWorldInitialized below).
        const std::vector<uintptr_t> WorldInitRefs = Finder::StringRefs(L"AFortNavigationGraph::OnWorldInitialized time:%.3fms memory:%.1fkB");
        const std::vector<uintptr_t> LoaderRefs = Finder::StringRefs(L"Misaligned terrain data found in level [%s]!");
        const uintptr_t WorldInit = WorldInitRefs.size() == 1 ? Finder::FunctionStart(WorldInitRefs[0]) : 0;
        const uintptr_t WorldInitEnd = WorldInit ? Finder::FunctionEnd(WorldInit) : 0;
        if (!WorldInitEnd || LoaderRefs.empty() || Natives::AFortNavigationGraph_Cells < 0)
        {
            Log::Info(L"Finder: AFortNavigationGraph's terrain loader not looked for (OnWorldInitialized {}, {} loader string refs), Core decodes the terrain", WorldInit != 0, LoaderRefs.size());
            return false;
        }

        // The register `this` is kept in: mov r64, rcx (REX.W 8B /r, rm = rcx).
        int32_t ThisRegister = -1;
        for (uintptr_t Site = WorldInit; Site + 3 <= WorldInit + 0x20 && ThisRegister < 0; ++Site)
        {
            const uint8_t Rex = Byte(Site);
            const uint8_t ModRm = Byte(Site + 2);
            if ((Rex == 0x48 || Rex == 0x4C) && Byte(Site + 1) == 0x8B && (ModRm & 0xC7) == 0xC1)
            {
                ThisRegister = ((ModRm >> 3) & 7) | (Rex & 0x04 ? 8 : 0);
            }
        }
        const auto IsLoader = [&LoaderRefs](uintptr_t Target)
        {
            for (const uintptr_t Ref : LoaderRefs)
            {
                if (Finder::FunctionStart(Ref) == Target)
                {
                    return true;
                }
                for (const auto& [Begin, End] : Finder::FunctionChunks(Target))
                {
                    if (Ref >= Begin && Ref < End)
                    {
                        return true;
                    }
                }
            }
            return false;
        };
        // mov <reg>, <reg> with REX.W: Rex is 48 or 49 (4C or 4D for r8 as the destination), Reg the destination.
        const auto IsMovTo = [](uintptr_t Site, bool bExtendedDestination, uint8_t Reg, int32_t& OutSource)
        {
            const uint8_t Rex = Byte(Site);
            const uint8_t ModRm = Byte(Site + 2);
            if ((Rex & 0xFE) != (bExtendedDestination ? 0x4C : 0x48) || Byte(Site + 1) != 0x8B || (ModRm & 0xF8) != (0xC0 | (Reg << 3)))
            {
                return false;
            }
            OutSource = (ModRm & 7) | (Rex & 0x01 ? 8 : 0);
            return true;
        };
        // lea rcx, [<this>+Cells] (disp32), the cells' layout, within the 0x60 bytes before the loader's call (6.02: 0x35).
        const auto LoadsCells = [ThisRegister](uintptr_t CallSite)
        {
            for (uintptr_t Site = CallSite - 0x60; Site + 7 <= CallSite; ++Site)
            {
                const uint8_t Rex = Byte(Site);
                const uint8_t ModRm = Byte(Site + 2);
                if ((Rex == 0x48 || Rex == 0x49) && Byte(Site + 1) == 0x8D && (ModRm & 0xF8) == 0x88 && ((ModRm & 7) | (Rex & 0x01 ? 8 : 0)) == ThisRegister
                    && (ModRm & 7) != 4 && Displacement(Site + 3) == static_cast<int32_t>(Natives::AFortNavigationGraph_Cells))
                {
                    return true;
                }
            }
            return false;
        };

        uintptr_t Loader = 0;
        int32_t Matches = 0;
        for (const auto& [Site, Target] : ThisRegister >= 0 ? Finder::DirectCalls(WorldInit, WorldInitEnd) : std::vector<std::pair<uintptr_t, uintptr_t>>())
        {
            int32_t GraphSource = -1;
            int32_t Ignored = -1;
            if (Site >= WorldInit + 9 && IsLoader(Target) && IsMovTo(Site - 9, true, 0, GraphSource) && GraphSource == ThisRegister && IsMovTo(Site - 6, false, 2, Ignored)
                && IsMovTo(Site - 3, false, 1, Ignored) && LoadsCells(Site))
            {
                Loader = Target;
                ++Matches;
            }
        }
        if (Matches != 1)
        {
            Log::Info(L"Finder: AFortNavigationGraph's terrain loader not found in OnWorldInitialized (0x{:X}, {} matching calls, this in register {}), Core decodes the terrain",
                WorldInit - Finder::ImageBase(), Matches, ThisRegister);
            return false;
        }
        Natives::AFortNavigationGraph_LoadTerrainCells = reinterpret_cast<decltype(Natives::AFortNavigationGraph_LoadTerrainCells)>(Loader);
        Log::Info(L"Finder: AFortNavigationGraph's terrain loader 0x{:X}, called by OnWorldInitialized 0x{:X}", Loader - Finder::ImageBase(), WorldInit - Finder::ImageBase());
        return true;
    }();
    return bResolved;
}

int32_t AFortNavigationGraph::LoadTerrainCells(UWorld* World)
{
#if WITH_SERVER_CODE
    // #424: husks came up under the ground. The encounters' spawn queries (EncounterRandomDirectionQuery's
    // FortQueryGenerator_PointsFromNavGraph, GenerateItems 6.02 CL-4461277 0xA69A70) take only flat cells by default
    // (bOnlyFlatSurface, the class default's 0xCA at +0x198): a cell whose byte +18 is set is left out (0xA6A239). That
    // byte is the top byte of a WorldCells entry's location word, turned with the level, which the game's loader writes
    // with the rest of the entry: the ground links (+15), the steps (+6), the jumps (+16, the fourth word's low byte) and
    // bit 28 of the links word (+19), and it then links the cells the links word's bits 20-27 point to (0xAD59B0) and works
    // out the climbs (+17, 0xAE2B30). Core's decoding had the links and steps only, so a sloped terrain cell (a cave
    // ramp: Arid_3x3_Terrain_a's cell (0,0,1) has 0x6F there) counted as flat. The generator puts its points half a cell
    // up and projects them with a capsule trace down (ProjectionData: ExtentZ 160, PostProjectionVerticalOffset -140); on
    // a slope the capsule starts inside the ground, the trace hits where it starts, and the husk spawned 140 under it.
    AFortGameStateZone* GameState = World ? Cast<AFortGameStateZone>(reinterpret_cast<UObject*>(World->GetGameState())) : nullptr;
    UBuildingStructuralSupportSystem* SupportSystem = GameState ? static_cast<UBuildingStructuralSupportSystem*>(GameState->StructuralSupportSystem) : nullptr;
    if (!SupportSystem || static_cast<AFortNavigationGraph*>(SupportSystem->NavGraph) != this || !ResolveTerrainLoader())
    {
        return DecodeTerrainCells(World);
    }

    Natives::AFortNavigationGraph_LoadTerrainCells(World, SupportSystem, this);
    // The cells with ground navigation, as the game tests a cell for it (0xA781D0).
    int32_t Loaded = 0;
    for (const FNavGraphCell& Cell : Cells.Cells)
    {
        if (Cell.GroundLinks || Cell.GroundSteps || Cell.GroundJumps || (Cell.Reserved19 & 1))
        {
            ++Loaded;
        }
    }
    return Loaded;
#else
    return 0;
#endif
}

int32_t AFortNavigationGraph::DecodeTerrainCells(UWorld* World)
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
    Log::Info(L"AFortNavigationGraph: {} terrain cells ({}), {} cells in {} blocks", TerrainCells,
        Natives::AFortNavigationGraph_LoadTerrainCells ? L"the game's loader, cells with ground navigation" : L"Core's decoding", Grid.Cells.Num(), Blocks.Num());

    // Every block's links in one go, as Epic's OnWorldInitialized builds them (5.30 0xAF46E0: a loop over the blocks
    // calling the block link builder, then the log below, timed from its start): the intact dirty-block update (5.30
    // 0xAFD470) runs that builder for each dirty block, then empties the list.
    TArray<int32_t>& Dirty = DirtyBlocks;
    for (const int32_t Block : Blocks)
    {
        Dirty.Add(Block);
    }
    Natives::AFortNavigationGraph_UpdateDirtyBlocks(this);
    const double ElapsedMs = NowMs() - Started;

    GraphFlags = GraphFlags & ~SuspendBlockUpdates;

    const TArray<TArray<uint32_t>>& Links = BlockLinks;
    size_t Bytes = static_cast<size_t>(Grid.Cells.Max()) * sizeof(FNavGraphCell) + static_cast<size_t>(Grid.BlockCells.Max()) * sizeof(int32_t) + static_cast<size_t>(Links.Max()) * sizeof(TArray<uint32_t>);
    for (const TArray<uint32_t>& BlockLinkList : Links)
    {
        Bytes += static_cast<size_t>(BlockLinkList.Max()) * sizeof(uint32_t);
    }
    UE_LOG(LogNavigation, Log, TEXT("AFortNavigationGraph::OnWorldInitialized time:%.3fms memory:%.1fkB"), ElapsedMs, static_cast<double>(Bytes) / 1024.0);
#endif
}

std::wstring AFortNavigationGraph::DescribeCellAt(const FVector& Location, bool& bOutBuilt) const
{
    bOutBuilt = false;
    if (!AreCellsInitialized())
    {
        return L"no cells";
    }

    // The cell index FNavGraphCells::GetCellIndex works out (6.00 CL-4395664 0xB13940), without allocating the block it
    // allocates when the block is missing: block (x >> ShiftX) + BlocksX * ((y >> ShiftY) + BlocksY * (z >> ShiftZ)), then
    // the cell in it (x & MaskX) | (y & MaskY) << ShiftX | (z & MaskZ) << (ShiftX + ShiftY).
    const FNavGraphCells& Grid = Cells;
    const int32_t Coords[3] = {
        static_cast<int32_t>(std::floor(Location.X / CellSizeXY)) - Grid.Origin[0],
        static_cast<int32_t>(std::floor(Location.Y / CellSizeXY)) - Grid.Origin[1],
        static_cast<int32_t>(std::floor(Location.Z / CellSizeZ)) - Grid.Origin[2],
    };
    std::wstring Description = std::format(L"({}, {}, {})", Coords[0], Coords[1], Coords[2]);
    if (Coords[0] < 0 || Coords[0] >= Grid.Size[0] || Coords[1] < 0 || Coords[1] >= Grid.Size[1] || Coords[2] < 0 || Coords[2] >= Grid.Size[2])
    {
        return Description + L" outside the grid";
    }
    const int32_t Block = (Coords[0] >> Grid.BlockShift[0]) + Grid.Blocks[0] * ((Coords[1] >> Grid.BlockShift[1]) + Grid.Blocks[1] * (Coords[2] >> Grid.BlockShift[2]));
    const int32_t FirstCell = Block >= 0 && Block < Grid.BlockCells.Num() ? Grid.BlockCells[Block] : -1;
    if (FirstCell < 0)
    {
        return Description + std::format(L" in empty block {}", Block);
    }
    bOutBuilt = true;
    const int32_t CellIndex = FirstCell + ((Coords[0] & Grid.BlockMask[0]) | ((Coords[1] & Grid.BlockMask[1]) << Grid.BlockShift[0]) | ((Coords[2] & Grid.BlockMask[2]) << (Grid.BlockShift[0] + Grid.BlockShift[1])));
    if (CellIndex < 0 || CellIndex >= Grid.Cells.Num())
    {
        return Description + std::format(L" in block {}, cell {} past the {} cells", Block, CellIndex, Grid.Cells.Num());
    }
    const FNavGraphCell& Cell = Grid.Cells[CellIndex];
    const TArray<TArray<uint32_t>>& Links = BlockLinks;
    const int32_t BlockLinkCount = Block < Links.Num() ? Links[Block].Num() : -1;
    return Description + std::format(L" in block {} ({} links): ground links 0x{:02X}, steps 0x{:04X}, floors 0x{:02X}, walls 0x{:02X}, ramp/floor links {}/{}",
        Block, BlockLinkCount, Cell.GroundLinks, Cell.GroundSteps, Cell.Floors, Cell.Walls, Cell.RampLinkCount, Cell.FloorLinkCount);
}
