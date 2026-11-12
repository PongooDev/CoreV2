#include "pch.h"
#include "Athena/AI/AthenaNavSystem.h"
#include "AI/Navigation/NavMeshBoundsVolume.h"
#include "AI/Navigation/NavigationData.h"
#include "AI/Navigation/NavigationSystem.h"
#include "AI/Navigation/RecastNavMesh.h"
#include "Components/SceneComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Finder.h"
#include "GameFramework/WorldSettings.h"
#include "GameFramework/Volume.h"
#include "Log.h"
#include "Natives.h"
#include "PhysicsEngine/BodySetup.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"
#include "UObject/WeakObjectPtr.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <deque>
#include <format>
#include <functional>
#include <io.h>
#include <memory>
#include <mutex>
#include <share.h>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace
{
    // 8.30's invoker tiles: the bot's surroundings (Core's radii ðŸŸ¡; 21.40 keeps a 5000 uu removal radius on its boxes).
    constexpr float InvokerGenerationRadius = 4000.0f;
    constexpr float InvokerRemovalRadius = 6000.0f;
    // ENavDataGatheringModeConfig::Lazy and ERuntimeGenerationType::Dynamic.
    constexpr uint8_t GatheringLazy = 2;
    constexpr uint8_t RuntimeDynamic = 2;
    // Tile jobs at once (Core's ðŸŸ¡): each one's geometry is gathered on the game thread when it starts (see
    // InitializeForBots), so this bounds that work per frame; the tiles still build on the worker threads.
    constexpr int32_t MaxTileGenerationJobs = 8;

    bool bInitialized = false;
    bool bReady = false;
    bool bTried = false;
    FWeakObjectPtr NavDataPtr;

    double NowMs()
    {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    std::wstring NameOf(const UObject* Object)
    {
        return Object ? Object->GetName().ToStdString() : std::wstring(L"none");
    }

    // A pointer member of Object, by its reflected name (a property Core has no wrapper for).
    UObject** ObjectMember(UObject* Object, const wchar_t* Name)
    {
        UProperty* Property = Object ? Object->GetClass()->FindPropertyByName(FName(Name, FNAME_Find)) : nullptr;
        return Property && Property->ElementSize == sizeof(UObject*) ? reinterpret_cast<UObject**>(reinterpret_cast<uint8_t*>(Object) + Property->GetOffset_ForInternal()) : nullptr;
    }

    // The agent a Battle Royale player is: a recast navmesh's agent (not STW's building-grid graph) whose radius is
    // nearest the player capsule's ðŸŸ¡ (Fortnite's player capsule is 36 x 90 half-height).
    int32_t PickPlayerAgent(UNavigationSystem* NavSys)
    {
        static UClass* RecastClass = ARecastNavMesh::StaticClass();
        TStructArray<FNavDataConfig>& Agents = NavSys->SupportedAgents;
        int32_t Best = -1;
        float BestScore = 1.0e9f;
        for (int32_t Index = 0; Index < Agents.Num(); ++Index)
        {
            FNavDataConfig& Agent = Agents[Index];
            UClass* DataClass = Agent.NavigationDataClass;
            const float Radius = Agent.AgentRadius;
            Log::Info(L"Battle Royale navigation: agent {} {}: {}, radius {:.0f}, height {:.0f}", Index, static_cast<FName>(Agent.Name).ToString().ToStdString(), NameOf(DataClass), Radius, static_cast<float>(Agent.AgentHeight));
            if (!DataClass || !RecastClass || !DataClass->IsChildOf(RecastClass))
            {
                continue;
            }
            const float Score = std::fabs(Radius - 36.0f);
            if (Score < BestScore)
            {
                BestScore = Score;
                Best = Index;
            }
        }
        return Best;
    }

    // One NavMeshBoundsVolume over the island. Battle Royale's cook strips the brushes, and a spawned volume has none:
    // its brush component's bounds come from a one-box body setup instead (UBrushComponent::CalcBounds uses
    // BrushBodySetup without a brush), a box the island's size.
    ANavMeshBoundsVolume* SpawnIslandBounds(UWorld* World, const FBox& Island)
    {
        FActorSpawnParameters SpawnInfo;
        SpawnInfo.ObjectFlags = SpawnInfo.ObjectFlags | RF_Transient;
        SpawnInfo.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        ANavMeshBoundsVolume* Volume = World->SpawnActor<ANavMeshBoundsVolume>(ANavMeshBoundsVolume::StaticClass(), SpawnInfo);
        UPrimitiveComponent* BrushComponent = Volume ? reinterpret_cast<AVolume*>(Volume)->GetBrushComponent() : nullptr;
        UObject** BodySetupMember = BrushComponent ? ObjectMember(reinterpret_cast<UObject*>(BrushComponent), L"BrushBodySetup") : nullptr;
        if (!BodySetupMember)
        {
            Log::Warning(L"Battle Royale navigation: the bounds volume has no brush component to shape");
            return Volume;
        }
        UBodySetup* BodySetup = NewObject<UBodySetup>(reinterpret_cast<UObject*>(BrushComponent), UBodySetup::StaticClass());
        if (BodySetup)
        {
            FKBoxElem& Elem = BodySetup->AggGeom.BoxElems.AddDefaulted();
            // The box is the island's size at scale 1: FKAggregateGeom::CalcAABB scales box elements by the smallest
            // axis of the component's scale (the first run's island-scaled unit box came out a 22440 cube).
            const FVector Size = Island.Max - Island.Min;
            Elem.X = Size.X;
            Elem.Y = Size.Y;
            Elem.Z = Size.Z;
            *BodySetupMember = reinterpret_cast<UObject*>(BodySetup);
        }
        if (USceneComponent* Root = Volume->GetRootComponent())
        {
            Root->Mobility = EComponentMobility::Movable;
        }
        Volume->SetActorLocation(Island.GetCenter());
        return Volume;
    }

    // 3.0's Athena world is made without a navigation system: its world settings turn bEnableNavigationSystem off, so
    // UWorld::InitWorld and LoadMap's UNavigationSystem::InitializeForWorld skip CreateNavigationSystem. 8.30's Athena
    // world has one (UAthenaNavSystem, from its world settings and DefaultEngine's NavigationSystemClass). Core turns
    // the world's setting on and runs the engine's own InitializeForWorld(World, GameMode) as LoadMap does: it makes
    // GEngine's NavigationSystemClass for the world and runs its OnWorldInitDone. The class default's config is
    // 8.30's UAthenaNavSystemConfig for the new system's lifetime (invokers only, no data spawned for every agent, lazy
    // gathering), so the system is born with it, as from DefaultEngine.ini; the default is put back afterwards.
    //
    // 4.2 (UE 4.20's NavigationSystem module) makes the world's system from its world settings' NavigationSystemConfig
    // instead (FNavigationSystem::AddNavigationSystemToWorld, LoadMap's call in InitializeForWorld's place), and
    // Athena_Terrain's has none. The world settings get a UAthenaNavSystemConfig (8.30's Athena world settings have
    // one) that makes GEngine's NavigationSystemClass, and the engine's own AddNavigationSystemToWorld(World, GameMode)
    // runs as LoadMap does: UNavigationSystemConfig::CreateAndConfigureNavigationSystem, UWorld::SetNavigationSystem and
    // the system's InitializeForWorld.
    UClass* PrepareNavigationSystemConfig(AWorldSettings* Settings, UClass* EngineClass)
    {
        UNavigationSystemConfig* Config = Settings->NavigationSystemConfig;
        if (!Config)
        {
            UClass* AthenaConfigClass = FindObject<UClass>(nullptr, L"/Script/FortniteGame.AthenaNavSystemConfig");
            UClass* ConfigClass = AthenaConfigClass ? AthenaConfigClass : UNavigationSystemModuleConfig::StaticClass();
            Config = ConfigClass ? NewObject<UNavigationSystemConfig>(reinterpret_cast<UObject*>(Settings), ConfigClass) : nullptr;
            if (!Config)
            {
                return nullptr;
            }
            Settings->NavigationSystemConfig = Config;
        }
        const FString ClassPath = Config->NavigationSystemClass.ToString();
        UClass* NavSysClass = ClassPath.Len() > 0 ? FindObject<UClass>(nullptr, *ClassPath) : nullptr;
        if (!NavSysClass && EngineClass)
        {
            Config->NavigationSystemClass.SetPath(reinterpret_cast<UObject*>(EngineClass)->GetPathName());
            NavSysClass = EngineClass;
        }
        // No navigation data spawned for every agent by itself (UNavigationSystemV1::Configure copies it into the
        // system's bAutoCreateNavigationData).
        if (Config->IsA(UNavigationSystemModuleConfig::StaticClass()))
        {
            static_cast<UNavigationSystemModuleConfig*>(Config)->bAutoSpawnMissingNavData = false;
        }
        Log::Info(L"Battle Royale navigation: the world settings' navigation config is {}, for {}", NameOf(reinterpret_cast<UObject*>(Config)), NameOf(reinterpret_cast<UObject*>(NavSysClass)));
        return NavSysClass;
    }

    UNavigationSystem* CreateNavigationSystem(UWorld* World)
    {
        AWorldSettings* Settings = World->GetWorldSettings(false, false);
        const bool bFromConfig = AWorldSettings::HasNavigationSystemConfig();
        UClass* NavSysClass = GEngine->NavigationSystemClass;
        if (bFromConfig && Settings)
        {
            NavSysClass = Finder::ResolveAddNavigationSystemToWorldLive(UWorld::NavigationSystemOffset(), AWorldSettings::NavigationSystemConfigOffset())
                ? PrepareNavigationSystemConfig(Settings, NavSysClass) : nullptr;
        }
        UNavigationSystem* Default = NavSysClass ? reinterpret_cast<UNavigationSystem*>(NavSysClass->GetDefaultObject()) : nullptr;
        if (!Settings || !Default || (!bFromConfig && !Finder::ResolveNavigationSystemInitializeForWorldLive(UWorld::NavigationSystemOffset())))
        {
            Log::Warning(L"Battle Royale navigation: can't make the world's navigation system (settings {}, class {})", NameOf(reinterpret_cast<UObject*>(Settings)), NameOf(reinterpret_cast<UObject*>(NavSysClass)));
            return nullptr;
        }
        const bool bWasEnabled = Settings->bEnableNavigationSystem;
        const bool bWasInvokersOnly = Default->bGenerateNavigationOnlyAroundNavigationInvokers;
        const bool bWasAutoCreate = Default->bAutoCreateNavigationData;
        const uint8_t WasGathering = Default->DataGatheringMode;
        Settings->bEnableNavigationSystem = true;
        Default->bGenerateNavigationOnlyAroundNavigationInvokers = true;
        Default->bAutoCreateNavigationData = false;
        Default->DataGatheringMode = GatheringLazy;
        const double Started = NowMs();
        if (bFromConfig)
        {
            FNavigationSystem::AddNavigationSystemToWorld(*World, FNavigationSystemRunMode::GameMode);
        }
        else
        {
            UNavigationSystem::InitializeForWorld(World, FNavigationSystemRunMode::GameMode);
        }
        Default->bGenerateNavigationOnlyAroundNavigationInvokers = bWasInvokersOnly;
        Default->bAutoCreateNavigationData = bWasAutoCreate;
        Default->DataGatheringMode = WasGathering;
        UNavigationSystem* NavSys = World->GetNavigationSystem();
        Log::Info(L"Battle Royale navigation: the world's navigation system was {} (world settings had navigation {}); made {} in {:.0f} ms", NavSys ? L"made" : L"not made",
            bWasEnabled ? L"on" : L"off", NameOf(reinterpret_cast<UObject*>(NavSys)), NowMs() - Started);
        return NavSys;
    }

    // ---- The navmesh tile cache (Core's) ----
    //
    // A finished tile is one dtMeshTile data blob (dtCreateNavMeshData's, what AddGeneratedTiles hands dtNavMesh::addTile;
    // FPImplRecastNavMesh::Serialize, 3.1 0x1E29650, saves the same per tile). Core copies each one the engine builds for a
    // tile nobody has cached, writes it to the cache file on a worker thread, and adds the cached ones to the next matches'
    // navmesh. Detour keeps no pointers in the blob (addTile points the tile into it and rebuilds its links), so a copy taken
    // from the live tile adds again as it was built. Core keeps the data (flags 0, not DT_TILE_FREE_DATA): the engine's
    // removeTile then leaves it, and the same tile can be added again.
    //
    // The generator only builds tiles in its ActiveTiles (the invokers' tiles, bRestrictBuildingToActiveTiles), and
    // ARecastNavMesh::UpdateActiveTiles (3.1 0x1E30930) re-adds (ReAddTiles 0x1E51DD0, a full rebuild) every tile that
    // wasn't active at its last update. Core keeps the cached tiles around each bot in ActiveTiles ahead of the bot, so
    // the update finds them active and leaves them be; a structure's dirty area there still rebuilds them through the
    // engine's MarkDirtyTiles (they are active), and that rebuild is kept for this match only.
    //
    // File (little-endian), beside Core.dll in NavCache\:
    //   header: "CNAV", format version, changelist, map name (length, UTF-16), the navmesh's agent radius and height, tile
    //           size, cell size and height, Detour's origin and tile width and height, CRC32 of all of it
    //   records: "TILE", tile x, y, layer count, then per layer: layer, size, CRC32, the dtMeshTile data
    constexpr uint32_t TileCacheFormatVersion = 1;
    constexpr uint32_t TileCacheMagic = 'V' << 24 | 'A' << 16 | 'N' << 8 | 'C';
    constexpr uint32_t TileRecordMagic = 'E' << 24 | 'L' << 16 | 'I' << 8 | 'T';
    constexpr uint32_t MaxTileDataSize = 16 * 1024 * 1024;
    constexpr uint32_t MaxTileLayers = 64;
    // Core's ðŸŸ¡: how often the active tiles are looked at, how many cached tiles are added per frame at match start and
    // per look while the bots move, how often new tiles are written, and how long a tile that left the active ones is
    // watched for the engine removing it.
    constexpr double TileCacheScanIntervalMs = 250.0;
    constexpr int32_t TileCachePreloadPerFrame = 256;
    constexpr int32_t TileCacheAddsPerScan = 128;
    constexpr double TileCacheWriteIntervalMs = 5000.0;
    constexpr size_t TileCacheWriteBatch = 64;
    constexpr double TileCacheLeftActiveWatchMs = 3000.0;
    constexpr double TileCacheSummaryIntervalMs = 30000.0;

    // UE 4.20's Detour, the same in 2.5, 3.0, 3.1 and 3.2: dtNavMesh (dtNavMesh::init 3.1 0x1C7BB40, getTileRefAt 0x1C7B670)
    // and dtMeshTile (176 bytes; getTile 0x1C7B310) and dtMeshHeader.
    namespace Detour
    {
        constexpr ptrdiff_t NavMeshOrig = 28;
        constexpr ptrdiff_t NavMeshTileWidth = 40;
        constexpr ptrdiff_t NavMeshTileHeight = 44;
        constexpr ptrdiff_t NavMeshTileLutMask = 56;
        constexpr ptrdiff_t NavMeshPosLookup = 128;
        constexpr ptrdiff_t TileHeader = 8;
        constexpr ptrdiff_t TileData = 88;
        constexpr ptrdiff_t TileDataSize = 96;
        constexpr ptrdiff_t TileNext = 104;
        constexpr ptrdiff_t HeaderX = 8;
        constexpr ptrdiff_t HeaderY = 12;
        constexpr ptrdiff_t HeaderLayer = 16;
        constexpr uint32_t NavMeshMagic = 'D' << 24 | 'N' << 16 | 'A' << 8 | 'V';
        constexpr uint32_t Failure = 1u << 31;
        constexpr uint32_t WrongMagic = 1u << 0;
        constexpr uint32_t WrongVersion = 1u << 1;
    }

    // FIntPoint, the generator's ActiveTiles' element.
    struct FTileCoord
    {
        int32_t X = 0;
        int32_t Y = 0;
    };

    uint64_t TileKey(int32_t X, int32_t Y)
    {
        return static_cast<uint64_t>(static_cast<uint32_t>(X)) << 32 | static_cast<uint32_t>(Y);
    }

    int32_t TileKeyX(uint64_t Key)
    {
        return static_cast<int32_t>(static_cast<uint32_t>(Key >> 32));
    }

    int32_t TileKeyY(uint64_t Key)
    {
        return static_cast<int32_t>(static_cast<uint32_t>(Key));
    }

    using FTileData = std::shared_ptr<std::vector<uint8_t>>;

    struct FTileLayer
    {
        int32_t Layer = 0;
        FTileData Data;
    };

    // Every layer of one tile (x, y).
    using FTileLayers = std::vector<FTileLayer>;

    struct FTileCacheKey
    {
        uint32_t Changelist = 0;
        std::wstring Map;
        float AgentRadius = 0.0f;
        float AgentHeight = 0.0f;
        float TileSizeUU = 0.0f;
        float CellSize = 0.0f;
        float CellHeight = 0.0f;

        bool operator==(const FTileCacheKey& Other) const = default;
    };

    // dtNavMesh's origin and tile size (Recast coordinates): a tile's x and y are counted from them.
    struct FTileCacheParams
    {
        float Orig[3] = {};
        float TileWidth = 0.0f;
        float TileHeight = 0.0f;

        bool operator==(const FTileCacheParams& Other) const = default;
    };

    uint32_t Crc32(const uint8_t* Data, size_t Size, uint32_t Crc = 0)
    {
        static const std::array<uint32_t, 256> Table = []
        {
            std::array<uint32_t, 256> Result = {};
            for (uint32_t Index = 0; Index < 256; ++Index)
            {
                uint32_t Value = Index;
                for (int32_t Bit = 0; Bit < 8; ++Bit)
                {
                    Value = Value & 1 ? 0xEDB88320u ^ (Value >> 1) : Value >> 1;
                }
                Result[Index] = Value;
            }
            return Result;
        }();
        Crc = ~Crc;
        for (size_t Index = 0; Index < Size; ++Index)
        {
            Crc = Table[(Crc ^ Data[Index]) & 0xFF] ^ (Crc >> 8);
        }
        return ~Crc;
    }

    // The build's changelist, from its build version string in .rdata ("++Fortnite+Release-3.1-CL-3915963", FApp's);
    // 0 when it isn't there.
    uint32_t BuildChangelist()
    {
        static const uint32_t Changelist = []
        {
            static constexpr wchar_t Prefix[] = L"++Fortnite+";
            const uintptr_t Found = Finder::FindReadOnly(Prefix, sizeof(Prefix) - sizeof(wchar_t));
            std::wstring Version;
            for (const wchar_t* Char = reinterpret_cast<const wchar_t*>(Found); Found && Version.size() < 128 && Finder::IsReadable(Char, sizeof(wchar_t)) && *Char; ++Char)
            {
                Version.push_back(*Char);
            }
            const size_t At = Version.find(L"-CL-");
            return At == std::wstring::npos ? 0u : static_cast<uint32_t>(std::wcstoul(Version.c_str() + At + 4, nullptr, 10));
        }();
        return Changelist;
    }

    // Core.dll's folder: Core's own, kept between matches and launches.
    std::wstring CoreDirectory()
    {
        HMODULE Module = nullptr;
        wchar_t Path[MAX_PATH] = {};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&CoreDirectory), &Module)
            || !GetModuleFileNameW(Module, Path, MAX_PATH))
        {
            return std::wstring();
        }
        const std::wstring FullPath = Path;
        return FullPath.substr(0, FullPath.find_last_of(L"\\/") + 1);
    }

    std::wstring TileCachePath(const FTileCacheKey& Key)
    {
        const std::wstring Directory = CoreDirectory();
        if (Directory.empty())
        {
            return std::wstring();
        }
        std::wstring Map = Key.Map;
        std::replace_if(Map.begin(), Map.end(), [](wchar_t Char) { return !std::iswalnum(Char) && Char != L'_' && Char != L'-'; }, L'_');
        return std::format(L"{}NavCache\\{}-{}-R{:.0f}-H{:.0f}.navcache", Directory, Key.Changelist, Map, Key.AgentRadius, Key.AgentHeight);
    }

    // Core's one thread for the cache's file: reading it and appending to it, in order, off the game thread.
    class FTileCacheWorker
    {
    public:
        static void Enqueue(std::function<void()> Job)
        {
            // Never destroyed: the thread waits on it until the process exits.
            static FTileCacheWorker* Worker = new FTileCacheWorker();
            {
                std::scoped_lock Lock(Worker->Mutex);
                Worker->Jobs.push_back(std::move(Job));
            }
            Worker->Wake.notify_one();
        }

    private:
        FTileCacheWorker()
        {
            std::thread([this] { Run(); }).detach();
        }

        void Run()
        {
            for (;;)
            {
                std::function<void()> Job;
                {
                    std::unique_lock Lock(Mutex);
                    Wake.wait(Lock, [this] { return !Jobs.empty(); });
                    Job = std::move(Jobs.front());
                    Jobs.pop_front();
                }
                Job();
            }
        }

        std::mutex Mutex;
        std::condition_variable Wake;
        std::deque<std::function<void()>> Jobs;
    };

    // The file's header, without its CRC.
    std::vector<uint8_t> TileCacheHeader(const FTileCacheKey& Key, const FTileCacheParams& Params)
    {
        std::vector<uint8_t> Bytes;
        const auto Put = [&Bytes](const void* Value, size_t Size) { Bytes.insert(Bytes.end(), static_cast<const uint8_t*>(Value), static_cast<const uint8_t*>(Value) + Size); };
        const uint32_t MapLength = static_cast<uint32_t>(Key.Map.size());
        Put(&TileCacheMagic, 4);
        Put(&TileCacheFormatVersion, 4);
        Put(&Key.Changelist, 4);
        Put(&MapLength, 4);
        Put(Key.Map.data(), MapLength * sizeof(wchar_t));
        for (const float Value : { Key.AgentRadius, Key.AgentHeight, Key.TileSizeUU, Key.CellSize, Key.CellHeight, Params.Orig[0], Params.Orig[1], Params.Orig[2], Params.TileWidth, Params.TileHeight })
        {
            Put(&Value, 4);
        }
        return Bytes;
    }

    // What the worker read from the file, for the game thread.
    struct FTileCacheRead
    {
        std::atomic<bool> bDone = false;
        bool bFound = false;
        bool bMatches = false;
        std::wstring Problem;
        FTileCacheParams Params;
        std::vector<std::pair<uint64_t, FTileLayers>> Tiles;
        size_t Layers = 0;
        size_t Bytes = 0;
        size_t DroppedBytes = 0;
        double Ms = 0.0;
    };

    // The worker's read: the header must be this key's exactly, each record whole with its CRCs. A record that isn't
    // (the server stopped while writing it, or the file is damaged) ends the read, and the file is cut back to the records
    // before it, so the next writes follow good ones.
    void ReadTileCache(const std::wstring& Path, const FTileCacheKey& Key, FTileCacheRead& Read)
    {
        const double Started = NowMs();
        FILE* File = _wfsopen(Path.c_str(), L"rb", _SH_DENYWR);
        if (!File)
        {
            Read.Ms = NowMs() - Started;
            return;
        }
        Read.bFound = true;
        const auto Get = [File](void* Value, size_t Size) { return std::fread(Value, 1, Size, File) == Size; };
        uint32_t Magic = 0;
        uint32_t Version = 0;
        FTileCacheKey FileKey;
        uint32_t MapLength = 0;
        bool bHeader = Get(&Magic, 4) && Magic == TileCacheMagic && Get(&Version, 4) && Get(&FileKey.Changelist, 4) && Get(&MapLength, 4) && MapLength < 256;
        if (bHeader)
        {
            FileKey.Map.resize(MapLength);
            bHeader = Get(FileKey.Map.data(), MapLength * sizeof(wchar_t));
        }
        for (float* Value : { &FileKey.AgentRadius, &FileKey.AgentHeight, &FileKey.TileSizeUU, &FileKey.CellSize, &FileKey.CellHeight, &Read.Params.Orig[0], &Read.Params.Orig[1], &Read.Params.Orig[2], &Read.Params.TileWidth, &Read.Params.TileHeight })
        {
            bHeader = bHeader && Get(Value, 4);
        }
        uint32_t HeaderCrc = 0;
        bHeader = bHeader && Get(&HeaderCrc, 4);
        if (bHeader && Version == TileCacheFormatVersion)
        {
            const std::vector<uint8_t> Expected = TileCacheHeader(FileKey, Read.Params);
            bHeader = Crc32(Expected.data(), Expected.size()) == HeaderCrc;
        }
        if (!bHeader || Version != TileCacheFormatVersion)
        {
            Read.Problem = !bHeader ? L"its header is damaged" : std::format(L"it is format {} (Core writes {})", Version, TileCacheFormatVersion);
        }
        else if (!(FileKey == Key))
        {
            Read.Problem = std::format(L"it is for CL {} {} (radius {:.0f}, height {:.0f}, tile {:.0f}, cell {:.1f} x {:.1f})", FileKey.Changelist, FileKey.Map, FileKey.AgentRadius, FileKey.AgentHeight, FileKey.TileSizeUU,
                FileKey.CellSize, FileKey.CellHeight);
        }
        if (!Read.Problem.empty())
        {
            std::fclose(File);
            Read.Ms = NowMs() - Started;
            return;
        }
        Read.bMatches = true;

        long long ValidEnd = _ftelli64(File);
        for (;;)
        {
            uint32_t RecordMagic = 0;
            int32_t X = 0;
            int32_t Y = 0;
            uint32_t LayerCount = 0;
            if (!Get(&RecordMagic, 4))
            {
                break;
            }
            if (RecordMagic != TileRecordMagic || !Get(&X, 4) || !Get(&Y, 4) || !Get(&LayerCount, 4) || LayerCount == 0 || LayerCount > MaxTileLayers)
            {
                break;
            }
            FTileLayers Layers;
            bool bWhole = true;
            for (uint32_t Index = 0; Index < LayerCount && bWhole; ++Index)
            {
                FTileLayer Layer;
                uint32_t Size = 0;
                uint32_t Crc = 0;
                bWhole = Get(&Layer.Layer, 4) && Get(&Size, 4) && Get(&Crc, 4) && Size >= 4 && Size <= MaxTileDataSize;
                if (bWhole)
                {
                    Layer.Data = std::make_shared<std::vector<uint8_t>>(Size);
                    bWhole = Get(Layer.Data->data(), Size) && Crc32(Layer.Data->data(), Size) == Crc && *reinterpret_cast<const uint32_t*>(Layer.Data->data()) == Detour::NavMeshMagic;
                    Read.Bytes += Size;
                }
                Layers.push_back(std::move(Layer));
            }
            if (!bWhole)
            {
                break;
            }
            Read.Layers += Layers.size();
            Read.Tiles.emplace_back(TileKey(X, Y), std::move(Layers));
            ValidEnd = _ftelli64(File);
        }
        _fseeki64(File, 0, SEEK_END);
        const long long FileEnd = _ftelli64(File);
        std::fclose(File);
        if (FileEnd > ValidEnd)
        {
            Read.DroppedBytes = static_cast<size_t>(FileEnd - ValidEnd);
            if (FILE* Cut = _wfsopen(Path.c_str(), L"r+b", _SH_DENYRW))
            {
                _chsize_s(_fileno(Cut), ValidEnd);
                std::fclose(Cut);
            }
        }
        Read.Ms = NowMs() - Started;
    }

    // The worker's write: the new tiles appended (after a header when the file is new, or started over when it was
    // another key's).
    void WriteTileCache(const std::wstring& Path, const FTileCacheKey& Key, const FTileCacheParams& Params, bool bFresh, const std::vector<std::pair<uint64_t, FTileLayers>>& Tiles)
    {
        const double Started = NowMs();
        CreateDirectoryW(Path.substr(0, Path.find_last_of(L"\\/")).c_str(), nullptr);
        FILE* File = _wfsopen(Path.c_str(), bFresh ? L"wb" : L"ab", _SH_DENYWR);
        if (!File)
        {
            Log::Warning(L"Battle Royale navigation: can't write the navmesh cache {} ({} tiles not saved)", Path, Tiles.size());
            return;
        }
        _fseeki64(File, 0, SEEK_END);
        if (_ftelli64(File) == 0)
        {
            const std::vector<uint8_t> Header = TileCacheHeader(Key, Params);
            const uint32_t HeaderCrc = Crc32(Header.data(), Header.size());
            std::fwrite(Header.data(), 1, Header.size(), File);
            std::fwrite(&HeaderCrc, 1, 4, File);
        }
        size_t Bytes = 0;
        std::vector<uint8_t> Record;
        for (const auto& [Key2, Layers] : Tiles)
        {
            Record.clear();
            const auto Put = [&Record](const void* Value, size_t Size) { Record.insert(Record.end(), static_cast<const uint8_t*>(Value), static_cast<const uint8_t*>(Value) + Size); };
            const int32_t X = TileKeyX(Key2);
            const int32_t Y = TileKeyY(Key2);
            const uint32_t LayerCount = static_cast<uint32_t>(Layers.size());
            Put(&TileRecordMagic, 4);
            Put(&X, 4);
            Put(&Y, 4);
            Put(&LayerCount, 4);
            for (const FTileLayer& Layer : Layers)
            {
                const uint32_t Size = static_cast<uint32_t>(Layer.Data->size());
                const uint32_t Crc = Crc32(Layer.Data->data(), Size);
                Put(&Layer.Layer, 4);
                Put(&Size, 4);
                Put(&Crc, 4);
                Put(Layer.Data->data(), Size);
            }
            // One fwrite per tile: a server stopped mid-write leaves at most this record cut short.
            std::fwrite(Record.data(), 1, Record.size(), File);
            Bytes += Record.size();
        }
        std::fclose(File);
        Log::Info(L"Battle Royale navigation: {} {} new navmesh tiles ({} KB) to the cache in {:.0f} ms", bFresh ? L"started the cache file with" : L"saved", Tiles.size(), Bytes / 1024, NowMs() - Started);
    }

    // The cache: the process's cached tiles for its key (read once, added to every match's navmesh), and this match's.
    struct FTileCache
    {
        FTileCacheKey Key;
        bool bHasKey = false;
        std::wstring Path;
        std::shared_ptr<FTileCacheRead> PendingRead;
        // The file was read (or there was none): Base is what it had, plus what this process saved since.
        bool bRead = false;
        bool bFileParamsKnown = false;
        FTileCacheParams FileParams;
        // The next write starts the file over: there is none yet, or it was another key's or damaged.
        bool bWriteFresh = true;
        // The base map's tiles, by (x, y).
        std::unordered_map<uint64_t, FTileLayers> Base;

        // This match's.
        bool bDisabled = false;
        ANavigationData* NavData = nullptr;
        uint8_t* DetourNavMesh = nullptr;
        uint8_t* Generator = nullptr;
        ptrdiff_t GeneratorOffset = -1;
        bool bParamsKnown = false;
        FTileCacheParams Params;
        // Tiles the engine built again this match (a structure's dirty area, or a tile Core didn't keep active): this
        // match's version, put back if the engine drops the tile. Empty: the engine left no navmesh there.
        std::unordered_map<uint64_t, FTileLayers> Latest;
        // Tiles a structure changed this match (AddDirtyArea): Changed until the engine builds them again, Touched for
        // the rest of the match (never saved as the base map's).
        std::unordered_set<uint64_t> Changed;
        std::unordered_set<uint64_t> Touched;
        std::vector<FBox> PendingDirtyAreas;
        // The tile data at each looked-at tile when Core last looked, to tell the engine's new tiles.
        std::unordered_map<uint64_t, std::vector<const void*>> Seen;
        // Core's tile data in the navmesh, kept alive while it is there.
        std::unordered_map<const void*, FTileData> InMesh;
        // The last match's, freed one match later (its navmesh may outlive its world for a while).
        std::vector<FTileData> Retired;
        std::unordered_set<uint64_t> LastActive;
        std::unordered_map<uint64_t, double> LeftActive;
        std::vector<FWeakObjectPtr> Invokers;
        std::vector<uint64_t> PreloadQueue;
        size_t PreloadNext = 0;
        bool bPreloadStarted = false;
        bool bPreloadDone = false;
        double PreloadStarted = 0.0;
        double PreloadAddMs = 0.0;
        int32_t PreloadFrames = 0;
        int32_t PreloadAdded = 0;
        int32_t PreloadFailed = 0;
        std::vector<std::pair<uint64_t, FTileLayers>> Unwritten;
        double LastWrite = 0.0;
        double NextScan = 0.0;
        double NextSummary = 0.0;
        int32_t TilesSaved = 0;
        int32_t TilesRebuilt = 0;
        int32_t TilesRestored = 0;
        // Cached tiles Core kept active ahead of a bot (each one the engine would have built again).
        std::unordered_set<uint64_t> KeptActive;
        int32_t AddFailures = 0;
    };
    FTileCache TileCache;

    // The tile data at (X, Y), every layer: dtNavMesh's position lookup (getTileRefAt's hash and chain).
    template <typename FunctionType>
    void ForEachTileAt(const uint8_t* NavMesh, int32_t X, int32_t Y, FunctionType&& Visit)
    {
        const uint32_t Mask = static_cast<uint32_t>(*reinterpret_cast<const int32_t*>(NavMesh + Detour::NavMeshTileLutMask));
        const uint8_t* const* Lookup = *reinterpret_cast<const uint8_t* const* const*>(NavMesh + Detour::NavMeshPosLookup);
        if (!Lookup)
        {
            return;
        }
        const uint32_t Hash = (0x8DA6B343u * static_cast<uint32_t>(X) + 0xD8163841u * static_cast<uint32_t>(Y)) & Mask;
        for (const uint8_t* Tile = Lookup[Hash]; Tile; Tile = *reinterpret_cast<const uint8_t* const*>(Tile + Detour::TileNext))
        {
            const uint8_t* Header = *reinterpret_cast<const uint8_t* const*>(Tile + Detour::TileHeader);
            if (Header && *reinterpret_cast<const int32_t*>(Header + Detour::HeaderX) == X && *reinterpret_cast<const int32_t*>(Header + Detour::HeaderY) == Y)
            {
                Visit(*reinterpret_cast<const int32_t*>(Header + Detour::HeaderLayer), *reinterpret_cast<const uint8_t* const*>(Tile + Detour::TileData), *reinterpret_cast<const int32_t*>(Tile + Detour::TileDataSize));
            }
        }
    }

    bool HasTileAt(const uint8_t* NavMesh, uint64_t Key)
    {
        bool bAny = false;
        ForEachTileAt(NavMesh, TileKeyX(Key), TileKeyY(Key), [&bAny](int32_t, const uint8_t*, int32_t) { bAny = true; });
        return bAny;
    }

    TArray<FTileCoord>& ActiveTilesOf(uint8_t* Generator)
    {
        return *reinterpret_cast<TArray<FTileCoord>*>(Generator + Natives::FRecastNavMeshGenerator_ActiveTiles);
    }

    // dtNavMesh::addTile with Core's data (flags 0: Core keeps it), counted as AddGeneratedTiles counts its tiles.
    uint32_t AddCachedTile(const FTileData& Data)
    {
        uint64_t Ref = 0;
        const uint32_t Status = Natives::dtNavMesh_addTile(TileCache.DetourNavMesh, Data->data(), static_cast<int32_t>(Data->size()), 0, 0, &Ref);
        if (!(Status & Detour::Failure))
        {
            TileCache.InMesh[Data->data()] = Data;
            ++*reinterpret_cast<int32_t*>(TileCache.Generator + Natives::FRecastNavMeshGenerator_NumActiveTiles);
        }
        return Status;
    }

    // A wrong-magic or wrong-version tile means the cached data isn't this build's Detour's: the cache is dropped.
    void OnCachedTileRefused(uint32_t Status)
    {
        ++TileCache.AddFailures;
        if (Status & (Detour::WrongMagic | Detour::WrongVersion))
        {
            Log::Warning(L"Battle Royale navigation: dtNavMesh::addTile refused a cached tile (status 0x{:X}): the navmesh cache is dropped and starts over", Status);
            TileCache.Base.clear();
            TileCache.Unwritten.clear();
            TileCache.PreloadQueue.clear();
            TileCache.bWriteFresh = true;
        }
    }

    // Puts every layer of Layers into the navmesh; false when one was refused.
    bool AddCachedTiles(const FTileLayers& Layers)
    {
        bool bAdded = true;
        for (const FTileLayer& Layer : Layers)
        {
            const uint32_t Status = AddCachedTile(Layer.Data);
            if (Status & Detour::Failure)
            {
                OnCachedTileRefused(Status);
                bAdded = false;
            }
        }
        return bAdded;
    }

    const FTileLayers* CachedTile(uint64_t Key)
    {
        const auto Latest = TileCache.Latest.find(Key);
        if (Latest != TileCache.Latest.end())
        {
            return &Latest->second;
        }
        const auto Base = TileCache.Base.find(Key);
        return Base != TileCache.Base.end() ? &Base->second : nullptr;
    }

    // Tile coordinates under an unreal box (Recast's x is -X, its z is -Y).
    void MarkDirtyTiles(const FBox& Area)
    {
        const FTileCacheParams& Params = TileCache.Params;
        const int32_t MinX = static_cast<int32_t>(std::floor((-Area.Max.X - Params.Orig[0]) / Params.TileWidth));
        const int32_t MaxX = static_cast<int32_t>(std::floor((-Area.Min.X - Params.Orig[0]) / Params.TileWidth));
        const int32_t MinY = static_cast<int32_t>(std::floor((-Area.Max.Y - Params.Orig[2]) / Params.TileHeight));
        const int32_t MaxY = static_cast<int32_t>(std::floor((-Area.Min.Y - Params.Orig[2]) / Params.TileHeight));
        for (int32_t X = MinX; X <= MaxX && MaxX - MinX < 64; ++X)
        {
            for (int32_t Y = MinY; Y <= MaxY && MaxY - MinY < 64; ++Y)
            {
                TileCache.Changed.insert(TileKey(X, Y));
                TileCache.Touched.insert(TileKey(X, Y));
            }
        }
    }

    // Looks at the tiles at Key. Tile data that isn't Core's is the engine's new tile there: saved when nobody cached the
    // tile and no structure changed it this match, else kept as this match's version. bActive: Key is in ActiveTiles,
    // where the engine removes a tile only when its rebuild left no navmesh.
    void InspectTile(uint64_t Key, bool bActive)
    {
        struct FPresentTile
        {
            int32_t Layer;
            const uint8_t* Data;
            int32_t Size;
        };
        // Reused: thousands of tiles are looked at each time, nearly all unchanged.
        static std::vector<FPresentTile> Tiles;
        static std::vector<const void*> Present;
        Tiles.clear();
        Present.clear();
        ForEachTileAt(TileCache.DetourNavMesh, TileKeyX(Key), TileKeyY(Key), [](int32_t Layer, const uint8_t* Data, int32_t Size)
        {
            Tiles.push_back({ Layer, Data, Size });
            Present.push_back(Data);
        });
        std::sort(Present.begin(), Present.end());
        const auto SeenIt = TileCache.Seen.find(Key);
        if (SeenIt == TileCache.Seen.end() ? Present.empty() : SeenIt->second == Present)
        {
            return;
        }
        std::vector<const void*>& Previous = SeenIt == TileCache.Seen.end() ? TileCache.Seen[Key] : SeenIt->second;
        FTileLayers Layers;
        bool bEngineBuilt = false;
        for (const FPresentTile& Tile : Tiles)
        {
            if (TileCache.InMesh.contains(Tile.Data))
            {
                continue;
            }
            bEngineBuilt = true;
            if (Tile.Data && Tile.Size >= 4 && static_cast<uint32_t>(Tile.Size) <= MaxTileDataSize && *reinterpret_cast<const uint32_t*>(Tile.Data) == Detour::NavMeshMagic)
            {
                Layers.push_back({ Tile.Layer, std::make_shared<std::vector<uint8_t>>(Tile.Data, Tile.Data + Tile.Size) });
            }
        }
        // Core's data the engine removed (a rebuild replaced it, or RemoveTiles) is out of the navmesh.
        for (const void* Data : Previous)
        {
            if (!std::binary_search(Present.begin(), Present.end(), Data))
            {
                TileCache.InMesh.erase(Data);
            }
        }
        const bool bHadTiles = !Previous.empty();
        Previous = Present;
        if (!bEngineBuilt)
        {
            if (Previous.empty() && bHadTiles && bActive)
            {
                // Rebuilt to nothing (an active tile is only removed by its rebuild).
                TileCache.Latest[Key] = FTileLayers();
                TileCache.Changed.erase(Key);
            }
            return;
        }
        TileCache.Changed.erase(Key);
        if (Layers.empty())
        {
            return;
        }
        // Layers Core put there that the engine's rebuild left are this match's version too (the data starts with its
        // dtMeshHeader).
        for (const void* Data : Previous)
        {
            const auto Ours = TileCache.InMesh.find(Data);
            if (Ours != TileCache.InMesh.end())
            {
                Layers.push_back({ *reinterpret_cast<const int32_t*>(Ours->second->data() + Detour::HeaderLayer), Ours->second });
            }
        }
        if (!TileCache.Base.contains(Key) && !TileCache.Touched.contains(Key) && !TileCache.bDisabled)
        {
            // A tile of the base map nobody cached: saved. The file gets its own copy (the navmesh changes the links in
            // the data it holds).
            FTileLayers Copy;
            for (const FTileLayer& Layer : Layers)
            {
                Copy.push_back({ Layer.Layer, std::make_shared<std::vector<uint8_t>>(*Layer.Data) });
            }
            TileCache.Base[Key] = std::move(Layers);
            TileCache.Unwritten.emplace_back(Key, std::move(Copy));
            TileCache.Latest.erase(Key);
            ++TileCache.TilesSaved;
        }
        else
        {
            TileCache.Latest[Key] = std::move(Layers);
            ++TileCache.TilesRebuilt;
        }
    }

    // The cached tiles within reach of each bot (the generation radius and a tile, inside the removal radius) are put in
    // the navmesh if they aren't there and added to ActiveTiles, ahead of UpdateActiveTiles: it finds them active and
    // doesn't rebuild them. Tiles a structure changed are left out, so the engine builds them for the bot.
    void KeepCachedTilesActiveAround(const FVector& Location, std::unordered_set<uint64_t>& Active, int32_t& Budget)
    {
        const FTileCacheParams& Params = TileCache.Params;
        const float Reach = (std::min)(InvokerGenerationRadius + Params.TileWidth, InvokerRemovalRadius - Params.TileWidth);
        const int32_t Span = static_cast<int32_t>(std::ceil(Reach / Params.TileWidth)) + 1;
        TArray<FTileCoord>& ActiveTiles = ActiveTilesOf(TileCache.Generator);
        const float RecastX = -Location.X - Params.Orig[0];
        const float RecastZ = -Location.Y - Params.Orig[2];
        const int32_t CenterX = static_cast<int32_t>(std::floor(RecastX / Params.TileWidth));
        const int32_t CenterY = static_cast<int32_t>(std::floor(RecastZ / Params.TileHeight));
        for (int32_t X = CenterX - Span; X <= CenterX + Span; ++X)
        {
            for (int32_t Y = CenterY - Span; Y <= CenterY + Span; ++Y)
            {
                const float DX = (static_cast<float>(X) + 0.5f) * Params.TileWidth - RecastX;
                const float DY = (static_cast<float>(Y) + 0.5f) * Params.TileHeight - RecastZ;
                const uint64_t Key = TileKey(X, Y);
                if (DX * DX + DY * DY > Reach * Reach || Active.contains(Key) || TileCache.Changed.contains(Key))
                {
                    continue;
                }
                const FTileLayers* Cached = CachedTile(Key);
                if (!Cached)
                {
                    continue;
                }
                if (!Cached->empty() && !HasTileAt(TileCache.DetourNavMesh, Key))
                {
                    if (Budget-- <= 0 || !AddCachedTiles(*Cached))
                    {
                        continue;
                    }
                    ++TileCache.TilesRestored;
                }
                ActiveTiles.Add(FTileCoord{ X, Y });
                Active.insert(Key);
                TileCache.KeptActive.insert(Key);
            }
        }
    }

    // The tile cache can work on this match's navmesh (its generator and dtNavMesh found, the cache's file read).
    bool IsTileCacheRunning()
    {
        return !TileCache.bDisabled && TileCache.bRead && TileCache.bParamsKnown && TileCache.DetourNavMesh && TileCache.Generator;
    }

    void KeepCachedTilesActive(std::unordered_set<uint64_t>& Active)
    {
        int32_t Budget = TileCacheAddsPerScan;
        std::erase_if(TileCache.Invokers, [](const FWeakObjectPtr& Invoker) { return !Invoker.Get(); });
        for (const FWeakObjectPtr& Invoker : TileCache.Invokers)
        {
            const AActor* Actor = reinterpret_cast<const AActor*>(Invoker.Get());
            KeepCachedTilesActiveAround(Actor ? Actor->GetTransform().GetLocation() : FVector(), Active, Budget);
        }
    }

    // A new invoker's cached tiles, made active at once: the navigation system's next UpdateInvokers would otherwise find
    // them new and rebuild them all (a bot landing).
    void KeepCachedTilesActiveForNewInvoker(const AActor* Invoker)
    {
        if (!Invoker || !IsTileCacheRunning())
        {
            return;
        }
        std::unordered_set<uint64_t> Active;
        for (const FTileCoord& Coord : ActiveTilesOf(TileCache.Generator))
        {
            Active.insert(TileKey(Coord.X, Coord.Y));
        }
        int32_t Budget = TileCacheAddsPerScan;
        KeepCachedTilesActiveAround(Invoker->GetTransform().GetLocation(), Active, Budget);
    }

    // Every TileCacheScanIntervalMs: the engine's new tiles in ActiveTiles, the tiles that left it (put back if the
    // engine removed them), and the bots' cached tiles kept active.
    void ScanTiles(double Now)
    {
        std::unordered_set<uint64_t> Active;
        for (const FTileCoord& Coord : ActiveTilesOf(TileCache.Generator))
        {
            const uint64_t Key = TileKey(Coord.X, Coord.Y);
            if (Active.insert(Key).second)
            {
                InspectTile(Key, true);
            }
        }
        for (const uint64_t Key : TileCache.LastActive)
        {
            if (!Active.contains(Key))
            {
                TileCache.LeftActive[Key] = Now + TileCacheLeftActiveWatchMs;
            }
        }
        for (auto It = TileCache.LeftActive.begin(); It != TileCache.LeftActive.end();)
        {
            const uint64_t Key = It->first;
            if (Active.contains(Key))
            {
                It = TileCache.LeftActive.erase(It);
                continue;
            }
            InspectTile(Key, false);
            // RemoveTiles took it out (its bot went beyond the removal radius): put back, so the island keeps it.
            const FTileLayers* Cached = CachedTile(Key);
            if (Cached && !Cached->empty() && !HasTileAt(TileCache.DetourNavMesh, Key) && AddCachedTiles(*Cached))
            {
                ++TileCache.TilesRestored;
            }
            It = Now > It->second ? TileCache.LeftActive.erase(It) : std::next(It);
        }
        KeepCachedTilesActive(Active);
        TileCache.LastActive = std::move(Active);
    }

    void FlushTileCache(bool bNow)
    {
        if (TileCache.Unwritten.empty() || !TileCache.bRead || !TileCache.bParamsKnown || TileCache.Path.empty()
            || (!bNow && TileCache.Unwritten.size() < TileCacheWriteBatch && NowMs() - TileCache.LastWrite < TileCacheWriteIntervalMs))
        {
            return;
        }
        auto Tiles = std::make_shared<std::vector<std::pair<uint64_t, FTileLayers>>>(std::move(TileCache.Unwritten));
        TileCache.Unwritten.clear();
        TileCache.LastWrite = NowMs();
        FTileCacheWorker::Enqueue([Path = TileCache.Path, Key = TileCache.Key, Params = TileCache.Params, bFresh = TileCache.bWriteFresh, Tiles]
        {
            WriteTileCache(Path, Key, Params, bFresh, *Tiles);
        });
        TileCache.bWriteFresh = false;
    }

    // The match is over: its navmesh goes with its world. The tiles stay for the next match.
    void ResetTileCacheForNewMatch()
    {
        FlushTileCache(true);
        TileCache.Retired.clear();
        for (auto& [Data, Tile] : TileCache.InMesh)
        {
            TileCache.Retired.push_back(std::move(Tile));
        }
        TileCache.InMesh.clear();
        TileCache.bDisabled = false;
        TileCache.NavData = nullptr;
        TileCache.DetourNavMesh = nullptr;
        TileCache.Generator = nullptr;
        TileCache.GeneratorOffset = -1;
        TileCache.bParamsKnown = false;
        TileCache.Latest.clear();
        TileCache.Changed.clear();
        TileCache.Touched.clear();
        TileCache.PendingDirtyAreas.clear();
        TileCache.Seen.clear();
        TileCache.LastActive.clear();
        TileCache.LeftActive.clear();
        TileCache.Invokers.clear();
        TileCache.PreloadQueue.clear();
        TileCache.PreloadNext = 0;
        TileCache.bPreloadStarted = false;
        TileCache.bPreloadDone = false;
        TileCache.PreloadAddMs = 0.0;
        TileCache.PreloadFrames = 0;
        TileCache.PreloadAdded = 0;
        TileCache.PreloadFailed = 0;
        TileCache.NextScan = 0.0;
        TileCache.NextSummary = 0.0;
        TileCache.TilesSaved = 0;
        TileCache.TilesRebuilt = 0;
        TileCache.TilesRestored = 0;
        TileCache.KeptActive.clear();
        TileCache.AddFailures = 0;
    }

    // The match's navmesh has its key: the file is read on the worker (once per process and key).
    void StartTileCache(UWorld* World, ANavigationData* NavData)
    {
        ARecastNavMesh* RecastNavMesh = reinterpret_cast<ARecastNavMesh*>(NavData);
        TileCache.NavData = NavData;
        if (!Finder::ResolveRecastNavMeshLive() || !ARecastNavMesh::HasTileSettings())
        {
            TileCache.bDisabled = true;
            Log::Warning(L"Battle Royale navigation: no navmesh cache on this build (addTile {}, tile settings {}); every tile is built", Natives::dtNavMesh_addTile != nullptr, ARecastNavMesh::HasTileSettings());
            return;
        }
        FTileCacheKey Key;
        Key.Changelist = BuildChangelist();
        Key.Map = NameOf(reinterpret_cast<UObject*>(World));
        Key.AgentRadius = RecastNavMesh->AgentRadius;
        Key.AgentHeight = RecastNavMesh->AgentHeight;
        Key.TileSizeUU = RecastNavMesh->TileSizeUU;
        Key.CellSize = RecastNavMesh->CellSize;
        Key.CellHeight = RecastNavMesh->CellHeight;
        if (Key.Changelist == 0)
        {
            TileCache.bDisabled = true;
            Log::Warning(L"Battle Royale navigation: the build's changelist isn't known, so no navmesh cache; every tile is built");
            return;
        }
        if (TileCache.bHasKey && TileCache.Key == Key)
        {
            Log::Info(L"Battle Royale navigation: navmesh cache {} has {} tiles in memory", TileCache.Path, TileCache.Base.size());
            return;
        }
        TileCache.Key = Key;
        TileCache.bHasKey = true;
        TileCache.Path = TileCachePath(Key);
        TileCache.bRead = false;
        TileCache.bFileParamsKnown = false;
        TileCache.bWriteFresh = true;
        TileCache.Base.clear();
        TileCache.Unwritten.clear();
        if (TileCache.Path.empty())
        {
            TileCache.bDisabled = true;
            Log::Warning(L"Battle Royale navigation: Core.dll's folder isn't known, so no navmesh cache");
            return;
        }
        Log::Info(L"Battle Royale navigation: navmesh cache {} (CL {}, {}, agent radius {:.0f} height {:.0f}, tile {:.0f}, cell {:.1f} x {:.1f}), reading it", TileCache.Path, Key.Changelist, Key.Map,
            Key.AgentRadius, Key.AgentHeight, Key.TileSizeUU, Key.CellSize, Key.CellHeight);
        auto Read = std::make_shared<FTileCacheRead>();
        TileCache.PendingRead = Read;
        FTileCacheWorker::Enqueue([Path = TileCache.Path, Key, Read]
        {
            ReadTileCache(Path, Key, *Read);
            Read->bDone = true;
        });
    }

    // The worker's read, taken in on the game thread.
    void TakeTileCacheRead()
    {
        if (!TileCache.PendingRead || !TileCache.PendingRead->bDone)
        {
            return;
        }
        const std::shared_ptr<FTileCacheRead> Read = std::move(TileCache.PendingRead);
        TileCache.bRead = true;
        if (!Read->bFound)
        {
            Log::Info(L"Battle Royale navigation: no navmesh cache yet: this match's tiles are built and saved");
            return;
        }
        if (!Read->bMatches)
        {
            Log::Warning(L"Battle Royale navigation: the navmesh cache is ignored and started over: {}", Read->Problem);
            return;
        }
        TileCache.bWriteFresh = false;
        TileCache.bFileParamsKnown = true;
        TileCache.FileParams = Read->Params;
        for (auto& [Key, Layers] : Read->Tiles)
        {
            TileCache.Base.try_emplace(Key, std::move(Layers));
        }
        Log::Info(L"Battle Royale navigation: navmesh cache read: {} tiles ({} layers, {:.1f} MB) in {:.0f} ms on a worker thread{}", TileCache.Base.size(), Read->Layers,
            static_cast<double>(Read->Bytes) / (1024.0 * 1024.0), Read->Ms, Read->DroppedBytes ? std::format(L"; {} bytes after the last whole tile were cut off", Read->DroppedBytes) : std::wstring());
    }

    // The generator, found from the navmesh: ANavigationData's native pointer whose FRecastNavMeshGenerator's DestNavMesh is
    // this navmesh (NavDataGenerator, a TSharedPtr; 3.1 +0x3B0, which ARecastNavMesh::UpdateActiveTiles reads).
    ptrdiff_t FindGeneratorOffset(ANavigationData* NavData)
    {
        const uint8_t* Base = reinterpret_cast<const uint8_t*>(NavData);
        for (ptrdiff_t Offset = 0x318; Offset < 0x5C8; Offset += sizeof(void*))
        {
            const uint8_t* Candidate = *reinterpret_cast<const uint8_t* const*>(Base + Offset);
            if (Candidate && Finder::IsReadable(Candidate + Natives::FRecastNavMeshGenerator_DestNavMesh, sizeof(void*))
                && *reinterpret_cast<ANavigationData* const*>(Candidate + Natives::FRecastNavMeshGenerator_DestNavMesh) == NavData
                && Finder::IsReadable(Candidate + Natives::FRecastNavMeshGenerator_ActiveTiles, sizeof(TArray<FTileCoord>)))
            {
                return Offset;
            }
        }
        return -1;
    }

    // Each frame once the navmesh is registered.
    void TickTileCache(ARecastNavMesh* RecastNavMesh)
    {
        if (TileCache.bDisabled || !RecastNavMesh)
        {
            return;
        }
        TakeTileCacheRead();
        uint8_t* DetourNavMesh = static_cast<uint8_t*>(RecastNavMesh->GetRecastMesh());
        // The generator is read through its pointer in the navmesh each frame (it can be made again, as the dtNavMesh).
        uint8_t* Generator = TileCache.GeneratorOffset > 0 ? *reinterpret_cast<uint8_t* const*>(reinterpret_cast<const uint8_t*>(TileCache.NavData) + TileCache.GeneratorOffset) : nullptr;
        if (!Generator || *reinterpret_cast<ANavigationData* const*>(Generator + Natives::FRecastNavMeshGenerator_DestNavMesh) != TileCache.NavData)
        {
            TileCache.GeneratorOffset = FindGeneratorOffset(TileCache.NavData);
            Generator = TileCache.GeneratorOffset > 0 ? *reinterpret_cast<uint8_t* const*>(reinterpret_cast<const uint8_t*>(TileCache.NavData) + TileCache.GeneratorOffset) : nullptr;
        }
        TileCache.Generator = Generator;
        if (!DetourNavMesh || !TileCache.Generator)
        {
            return;
        }
        if (DetourNavMesh != TileCache.DetourNavMesh)
        {
            // A new dtNavMesh (the generator made it again): none of Core's tiles are in it.
            if (TileCache.DetourNavMesh)
            {
                Log::Warning(L"Battle Royale navigation: the navmesh's dtNavMesh was made again; the cached tiles are added again");
            }
            for (auto& [Data, Tile] : TileCache.InMesh)
            {
                TileCache.Retired.push_back(std::move(Tile));
            }
            TileCache.InMesh.clear();
            TileCache.Seen.clear();
            TileCache.LastActive.clear();
            TileCache.LeftActive.clear();
            TileCache.bPreloadStarted = false;
            TileCache.bPreloadDone = false;
            TileCache.DetourNavMesh = DetourNavMesh;
            FTileCacheParams Params;
            std::memcpy(Params.Orig, DetourNavMesh + Detour::NavMeshOrig, sizeof(Params.Orig));
            Params.TileWidth = *reinterpret_cast<const float*>(DetourNavMesh + Detour::NavMeshTileWidth);
            Params.TileHeight = *reinterpret_cast<const float*>(DetourNavMesh + Detour::NavMeshTileHeight);
            // The generator's tile is a whole number of cells: FRecastNavMeshGenerator::Init sets the config's tileSize to
            // TruncToInt(TileSizeUU / CellSize), and the dtNavMesh's tile width is tileSize * cs (3.5.1's FortNavMesh:
            // 60 cells of 17, 1020, for a TileSizeUU of 1024; earlier builds' cells divide 1024).
            const float TileSizeUU = static_cast<float>(RecastNavMesh->TileSizeUU);
            const float CellSize = static_cast<float>(RecastNavMesh->CellSize);
            const float GeneratorTileSize = CellSize > 0.0f ? std::trunc(TileSizeUU / CellSize) * CellSize : TileSizeUU;
            if (std::fabs(Params.TileWidth - GeneratorTileSize) > 1.0f || std::fabs(Params.TileHeight - Params.TileWidth) > 1.0f)
            {
                // Not the dtNavMesh layout Core reads.
                TileCache.bDisabled = true;
                Log::Error(L"Battle Royale navigation: the dtNavMesh's tile size {:.0f} x {:.0f} isn't the navmesh's {:.0f} ({:.0f} in {:.1f} cells): no navmesh cache",
                    Params.TileWidth, Params.TileHeight, GeneratorTileSize, TileSizeUU, CellSize);
                return;
            }
            TileCache.Params = Params;
            TileCache.bParamsKnown = true;
            for (const FBox& Area : TileCache.PendingDirtyAreas)
            {
                MarkDirtyTiles(Area);
            }
            TileCache.PendingDirtyAreas.clear();
        }
        if (!TileCache.bRead)
        {
            return;
        }
        if (TileCache.bFileParamsKnown && !(TileCache.FileParams == TileCache.Params))
        {
            Log::Warning(L"Battle Royale navigation: the navmesh cache's origin ({:.0f}, {:.0f}, {:.0f}) and tiles {:.0f} aren't this navmesh's ({:.0f}, {:.0f}, {:.0f}) and {:.0f}: ignored and started over",
                TileCache.FileParams.Orig[0], TileCache.FileParams.Orig[1], TileCache.FileParams.Orig[2], TileCache.FileParams.TileWidth, TileCache.Params.Orig[0], TileCache.Params.Orig[1], TileCache.Params.Orig[2], TileCache.Params.TileWidth);
            TileCache.bFileParamsKnown = false;
            TileCache.Base.clear();
            TileCache.Unwritten.clear();
            TileCache.bWriteFresh = true;
        }
        const double Now = NowMs();

        // Match start: every cached tile into the navmesh, a few hundred a frame.
        if (!TileCache.bPreloadStarted)
        {
            TileCache.bPreloadStarted = true;
            TileCache.PreloadStarted = Now;
            TileCache.PreloadQueue.clear();
            TileCache.PreloadQueue.reserve(TileCache.Base.size());
            for (const auto& [Key, Layers] : TileCache.Base)
            {
                TileCache.PreloadQueue.push_back(Key);
            }
            TileCache.PreloadNext = 0;
        }
        if (!TileCache.bPreloadDone)
        {
            const double Started = NowMs();
            ++TileCache.PreloadFrames;
            for (int32_t Added = 0; Added < TileCachePreloadPerFrame && TileCache.PreloadNext < TileCache.PreloadQueue.size(); ++TileCache.PreloadNext)
            {
                const uint64_t Key = TileCache.PreloadQueue[TileCache.PreloadNext];
                const FTileLayers* Cached = CachedTile(Key);
                if (!Cached || Cached->empty() || HasTileAt(DetourNavMesh, Key))
                {
                    continue;
                }
                ++Added;
                if (AddCachedTiles(*Cached))
                {
                    ++TileCache.PreloadAdded;
                }
                else
                {
                    ++TileCache.PreloadFailed;
                }
            }
            TileCache.PreloadAddMs += NowMs() - Started;
            if (TileCache.PreloadNext >= TileCache.PreloadQueue.size())
            {
                TileCache.bPreloadDone = true;
                TileCache.PreloadQueue.clear();
                TileCache.PreloadQueue.shrink_to_fit();
                Log::Info(L"Battle Royale navigation: {} cached tiles loaded in {:.0f} ms ({:.0f} ms adding, over {} frames){}", TileCache.PreloadAdded, Now - TileCache.PreloadStarted, TileCache.PreloadAddMs,
                    TileCache.PreloadFrames, TileCache.PreloadFailed ? std::format(L", {} refused", TileCache.PreloadFailed) : std::wstring());
            }
        }

        if (Now >= TileCache.NextScan)
        {
            TileCache.NextScan = Now + TileCacheScanIntervalMs;
            ScanTiles(Now);
        }
        FlushTileCache(false);
        if (Now >= TileCache.NextSummary)
        {
            if (TileCache.NextSummary > 0.0 && (TileCache.TilesSaved || TileCache.TilesRebuilt || TileCache.TilesRestored))
            {
                Log::Info(L"Battle Royale navigation: navmesh cache this match: {} new tiles saved ({} cached), {} built again, {} cached tiles put back, {} kept active ahead of the bots{}", TileCache.TilesSaved,
                    TileCache.Base.size(), TileCache.TilesRebuilt, TileCache.TilesRestored, TileCache.KeptActive.size(), TileCache.AddFailures ? std::format(L", {} refused", TileCache.AddFailures) : std::wstring());
            }
            TileCache.NextSummary = Now + TileCacheSummaryIntervalMs;
        }
    }
}

bool UAthenaNavSystem::IsInitialized()
{
    return bInitialized;
}

void UAthenaNavSystem::ResetForNewMatch()
{
    bInitialized = false;
    bReady = false;
    bTried = false;
    NavDataPtr = FWeakObjectPtr();
    ResetTileCacheForNewMatch();
}

bool UAthenaNavSystem::InitializeForBots(UWorld* World, const FBox& IslandBounds)
{
#if WITH_SERVER_CODE
    if (bTried)
    {
        return bInitialized;
    }
    bTried = true;
    UNavigationSystem* NavSys = World ? World->GetNavigationSystem() : nullptr;
    if (!NavSys && World && IslandBounds.IsValid)
    {
        NavSys = CreateNavigationSystem(World);
    }
    if (!NavSys || !IslandBounds.IsValid || !UNavigationSystem::StaticFunction_RegisterNavigationInvoker())
    {
        Log::Warning(L"Battle Royale navigation: the world has no navigation system ({}), the bots steer without a navmesh", NameOf(reinterpret_cast<UObject*>(NavSys)));
        return false;
    }
    Log::Info(L"Battle Royale navigation: {} has {} nav data (main {}), {} agents, invokers only {}, auto create {}, gathering {}, build lock 0x{:X}", NameOf(reinterpret_cast<UObject*>(NavSys)),
        static_cast<TArray<ANavigationData*>&>(NavSys->NavDataSet).Num(), NameOf(reinterpret_cast<UObject*>(static_cast<ANavigationData*>(NavSys->MainNavData))), NavSys->SupportedAgents.Num(),
        NavSys->bGenerateNavigationOnlyAroundNavigationInvokers, NavSys->bAutoCreateNavigationData, static_cast<int32_t>(NavSys->DataGatheringMode), static_cast<int32_t>(NavSys->NavBuildingLockFlags));

    const int32_t AgentIndex = PickPlayerAgent(NavSys);
    if (AgentIndex < 0)
    {
        Log::Warning(L"Battle Royale navigation: no recast navmesh agent, the bots steer without a navmesh");
        return false;
    }
    FNavDataConfig& Agent = NavSys->SupportedAgents[AgentIndex];
    UClass* DataClass = Agent.NavigationDataClass;

    // 8.30's UAthenaNavSystemConfig: bUseNavigationInvokers and bLazyOctree. Set before any navmesh makes its generator
    // (FRecastNavMeshGenerator reads the invoker flag when it is made), and no other agent's data is spawned by itself.
    NavSys->bGenerateNavigationOnlyAroundNavigationInvokers = true;
    NavSys->bAutoCreateNavigationData = false;
    NavSys->DataGatheringMode = GatheringLazy;

    const double Started = NowMs();
    ANavMeshBoundsVolume* Volume = SpawnIslandBounds(World, IslandBounds);
    FVector Origin;
    FVector Extent;
    if (Volume)
    {
        Volume->GetActorBounds(false, Origin, Extent);
        const FVector Scale = Volume->GetActorScale3D();
        Log::Info(L"Battle Royale navigation: island ({:.0f}, {:.0f}, {:.0f}) - ({:.0f}, {:.0f}, {:.0f}); bounds volume at ({:.0f}, {:.0f}, {:.0f}), extent ({:.0f}, {:.0f}, {:.0f}), scale ({:.0f}, {:.0f}, {:.0f})",
            IslandBounds.Min.X, IslandBounds.Min.Y, IslandBounds.Min.Z, IslandBounds.Max.X, IslandBounds.Max.Y, IslandBounds.Max.Z, Origin.X, Origin.Y, Origin.Z, Extent.X, Extent.Y, Extent.Z, Scale.X, Scale.Y, Scale.Z);
        NavSys->OnNavigationBoundsUpdated(Volume);
    }
    if (!Volume || Extent.X < 1000.0f || Extent.Y < 1000.0f)
    {
        Log::Warning(L"Battle Royale navigation: the island's bounds volume has no size, the bots steer without a navmesh");
        return false;
    }

    // The navmesh, as AFortNavigationGraph::FindOrSpawn spawns the Zone's graph: the agent's class and config, made
    // dynamic (the runtime tiles need a generator), then registered.
    FActorSpawnParameters SpawnInfo;
    SpawnInfo.ObjectFlags = SpawnInfo.ObjectFlags | RF_Transient;
    SpawnInfo.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    ANavigationData* NavData = World->SpawnActor<ANavigationData>(DataClass, SpawnInfo);
    if (!NavData)
    {
        Log::Warning(L"Battle Royale navigation: couldn't spawn {}", NameOf(DataClass));
        return false;
    }
    FNavDataConfig::StaticStruct()->CopyScriptStruct(&NavData->NavDataConfig, &Agent);
    if (ANavigationData::HasRuntimeGeneration())
    {
        Log::Info(L"Battle Royale navigation: {} runtime generation {} -> dynamic", NameOf(NavData), static_cast<int32_t>(NavData->RuntimeGeneration));
        NavData->RuntimeGeneration = RuntimeDynamic;
    }
    // Each tile's geometry gathered on the game thread as its job is made (FRecastTileGenerator::Setup's GatherGeometry,
    // the engine's default), the build itself still on a worker thread, a few jobs at once. FortNavMesh gathers it on the tile
    // jobs' worker threads (bDoFullyAsyncNavDataGathering), and with lazy gathering a landscape component's slice for a
    // tile is written into the component's one shared FNavigationRelevantData (3.1's DoAsyncGeometryGathering 0x1E3F3B0:
    // GatherGeometrySlice, StoreCollisionCache appending to its CollisionData, then emptying it). Two jobs over the same
    // component (any two neighbouring tiles) race on it: the second append breaks the first's header, and
    // FNavigationRelevantData::IsCollisionDataValid (0x1E1D190) logs "NavOctree element has corrupted collision data!
    // Owner:LandscapeHeightfieldCollisionComponent_N Bounds:IsValid=false" (402 times in one 3.1 match) and the tile is
    // built without the ground. A tile emptied by the other job before its append loses the ground without the error.
    if (ARecastNavMesh::HasTileGenerationSettings())
    {
        ARecastNavMesh* RecastNavMesh = reinterpret_cast<ARecastNavMesh*>(NavData);
        const bool bWasAsync = RecastNavMesh->bDoFullyAsyncNavDataGathering;
        const int32_t WasJobs = RecastNavMesh->MaxSimultaneousTileGenerationJobsCount;
        RecastNavMesh->bDoFullyAsyncNavDataGathering = false;
        RecastNavMesh->MaxSimultaneousTileGenerationJobsCount = std::clamp(WasJobs, 1, MaxTileGenerationJobs);
        Log::Info(L"Battle Royale navigation: {} gathers tile geometry on the game thread (was {}), {} tile jobs at once (was {})", NameOf(NavData),
            bWasAsync ? L"on the jobs' threads" : L"on the game thread", static_cast<int32_t>(RecastNavMesh->MaxSimultaneousTileGenerationJobsCount), WasJobs);
    }
    else
    {
        Log::Warning(L"Battle Royale navigation: {} has no tile generation settings; its landscape tiles may be built without the ground", NameOf(NavData));
    }
    NavSys->RequestRegistration(NavData, true);
    NavDataPtr = FWeakObjectPtr(reinterpret_cast<UObject*>(NavData));
    Log::Info(L"Battle Royale navigation: {} for agent {} over the island, registering ({:.0f} ms)", NameOf(NavData), AgentIndex, NowMs() - Started);
    bInitialized = true;
    return true;
#else
    return false;
#endif
}

void UAthenaNavSystem::Tick(UWorld* World)
{
#if WITH_SERVER_CODE
    // The registration runs on the game thread's next task pass (RequestRegistration queues ProcessRegistrationCandidates).
    // Once the navmesh is registered (and so needs the octree: RequiresNavOctree), the octree (the geometry the tiles are
    // built from, gathered lazily) is populated, and the initial lock released: its RebuildAll only marks the active
    // tiles, which are the invokers'.
    if (!bInitialized)
    {
        return;
    }
    UNavigationSystem* NavSys = World ? World->GetNavigationSystem() : nullptr;
    ANavigationData* NavData = GetNavData(World);
    if (!NavSys || !NavData)
    {
        return;
    }
    if (bReady)
    {
        TickTileCache(reinterpret_cast<ARecastNavMesh*>(NavData));
        return;
    }
    bReady = true;
    NavSys->SetNavigationOctreeLock(false);
    const double OctreeStarted = NowMs();
    NavSys->ConditionalPopulateNavOctree();
    const double OctreeMs = NowMs() - OctreeStarted;
    const double LockStarted = NowMs();
    // The release rebuilds the navigation data when it is what unlocks the building (UNavigationSystem::
    // RemoveNavigationBuildLock). The game's own UAthenaNavSystem can get there first: on 6.02 (CL-4461277) its world
    // bring-up (0xAD7090: the pending bounds, OnWorldInitDone, ReleaseInitialBuildingLock, the octree) runs between
    // the navmesh's spawn and here, and its ReleaseInitialBuildingLock (0xADEC50, the navigation system's slot 0x58;
    // the same override is in 4.2 to 6.01) only clears the lock unless a flag no code left in the build sets is on. So
    // the lock was open already, and nothing rebuilt the navmesh. From 6.00 that matters: the generator's RebuildAll
    // (6.02 0x2C00C70) is the only code that gives it its origin (from the navmesh's NavMeshOriginOffset; its
    // constructor, 0x2BC2540, leaves RcNavMeshOrigin as the heap had it), and the dtNavMesh the first dirty tiles then
    // made took whatever was there: (0, 0, 1.5e28) one match, harmless denormals the next. The rebuild the release
    // left out runs here, the navigation system's own.
    const bool bWasLocked = NavSys->IsNavigationBuildingLocked();
    NavSys->ReleaseInitialBuildingLock();
    const bool bRebuildLeftOut = !bWasLocked && ARecastNavMesh::HasNavMeshOriginOffset();
    if (bRebuildLeftOut)
    {
        NavSys->RebuildAll();
    }
    Log::Info(L"Battle Royale navigation: {} registered (main {}); octree {:.0f} ms, lock release {:.0f} ms{}, build lock 0x{:X}", NameOf(NavData),
        NameOf(reinterpret_cast<UObject*>(static_cast<ANavigationData*>(NavSys->MainNavData))), OctreeMs, NowMs() - LockStarted,
        bRebuildLeftOut ? L" (already released by the game without a rebuild: rebuilt now)" : L"", static_cast<int32_t>(NavSys->NavBuildingLockFlags));
    // Registered, so its generator and dtNavMesh exist and its settings are the ones its tiles are built with.
    StartTileCache(World, NavData);
    if (bRebuildLeftOut)
    {
        // The dtNavMesh's origin in Recast's axes (-X, Z, -Y of NavMeshOriginOffset), where Core reads dtNavMesh.
        const FVector OriginOffset = reinterpret_cast<ARecastNavMesh*>(NavData)->NavMeshOriginOffset;
        const uint8_t* DetourNavMesh = static_cast<const uint8_t*>(reinterpret_cast<ARecastNavMesh*>(NavData)->GetRecastMesh());
        float Origin[3] = {};
        if (DetourNavMesh)
        {
            std::memcpy(Origin, DetourNavMesh + Detour::NavMeshOrig, sizeof(Origin));
        }
        Log::Info(L"Battle Royale navigation: {}'s origin offset is ({:g}, {:g}, {:g}){}", NameOf(NavData), OriginOffset.X, OriginOffset.Y, OriginOffset.Z,
            DetourNavMesh ? std::format(L", its dtNavMesh's origin ({:g}, {:g}, {:g})", Origin[0], Origin[1], Origin[2]) : std::wstring(L"; no dtNavMesh to read"));
    }
#endif
}

ANavigationData* UAthenaNavSystem::GetNavData(UWorld* World)
{
    ANavigationData* NavData = static_cast<ANavigationData*>(NavDataPtr.Get());
    UNavigationSystem* NavSys = World ? World->GetNavigationSystem() : nullptr;
    if (!NavData || !NavSys)
    {
        return nullptr;
    }
    TArray<ANavigationData*>& Registered = NavSys->NavDataSet;
    for (ANavigationData* Data : Registered)
    {
        if (Data == NavData)
        {
            return NavData;
        }
    }
    return nullptr;
}

void UAthenaNavSystem::RegisterNavigationInvoker(AActor* Invoker)
{
    UWorld* World = Invoker && bInitialized ? Invoker->GetWorld() : nullptr;
    if (UNavigationSystem* NavSys = World ? World->GetNavigationSystem() : nullptr)
    {
        NavSys->RegisterNavigationInvoker(Invoker, InvokerGenerationRadius, InvokerRemovalRadius);
        TileCache.Invokers.emplace_back(reinterpret_cast<UObject*>(Invoker));
        KeepCachedTilesActiveForNewInvoker(Invoker);
    }
}

void UAthenaNavSystem::AddDirtyArea(const AActor* Actor)
{
    // Before the navmesh exists too (the warmup's structures), up to a bound (modes without bots never take them).
    if (!Actor || TileCache.bDisabled || (!TileCache.bParamsKnown && TileCache.PendingDirtyAreas.size() >= 4096))
    {
        return;
    }
    FVector Origin;
    FVector Extent;
    Actor->GetActorBounds(false, Origin, Extent);
    // The agent's radius around it too: the navmesh keeps that far from geometry.
    const FVector Margin(TileCache.Key.AgentRadius, TileCache.Key.AgentRadius, 0.0f);
    const FBox Area(Origin - Extent - Margin, Origin + Extent + Margin);
    if (TileCache.bParamsKnown)
    {
        MarkDirtyTiles(Area);
    }
    else
    {
        TileCache.PendingDirtyAreas.push_back(Area);
    }
}

bool UAthenaNavSystem::ProjectPointToNavigation(UWorld* World, const FVector& Point, FVector& OutLocation, const FVector& Extent)
{
    ANavigationData* NavData = GetNavData(World);
    return NavData && UNavigationSystem::K2_ProjectPointToNavigation(reinterpret_cast<UObject*>(World), Point, OutLocation, NavData, Extent);
}

bool UAthenaNavSystem::IsOnNavMesh(UWorld* World, const FVector& Location)
{
    FVector Projected;
    return ProjectPointToNavigation(World, Location, Projected, FVector(100.0f, 100.0f, 250.0f));
}
