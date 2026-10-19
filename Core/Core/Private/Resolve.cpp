#include "pch.h"
#include "Finder.h"
#include "Natives.h"
#include "Log.h"

#include <algorithm>

namespace
{
    int32_t MissingCount = 0;

    uintptr_t First(const std::vector<uintptr_t>& Items)
    {
        return Items.empty() ? 0 : Items.front();
    }

    uintptr_t OnlyRef(const wchar_t* Text)
    {
        const std::vector<uintptr_t> Refs = Finder::StringRefs(Text);
        if (Refs.size() != 1)
        {
            Log::Warning(L"Finder: expected 1 reference to a string, found {}", Refs.size());
        }
        return First(Refs);
    }

    uintptr_t FunctionOf(const wchar_t* Text)
    {
        return Finder::FunctionStart(First(Finder::StringRefs(Text)));
    }

    template <typename T>
    void Require(const wchar_t* Name, T& Destination, uintptr_t Address)
    {
        if (!Address)
        {
            Log::Error(L"Finder: {} not found", Name);
            ++MissingCount;
            return;
        }
        Destination = reinterpret_cast<T>(Address);
    }

    void RequireAddress(const wchar_t* Name, uintptr_t& Destination, uintptr_t Address)
    {
        if (!Address)
        {
            Log::Error(L"Finder: {} not found", Name);
            ++MissingCount;
            return;
        }
        Destination = Address;
    }

    uint8_t Byte(uintptr_t Address)
    {
        return *reinterpret_cast<const uint8_t*>(Address);
    }

    uintptr_t FindInstruction(uintptr_t Begin, uintptr_t End, std::initializer_list<int32_t> Pattern)
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

    uintptr_t FindRawCall(uintptr_t Begin, uintptr_t End)
    {
        for (uintptr_t Address = Begin; Address + 5 <= End; ++Address)
        {
            if (Byte(Address) == 0xE8)
            {
                return Address;
            }
        }
        return 0;
    }

    bool CallWithin(uintptr_t Begin, size_t MaxDistance, uintptr_t Target)
    {
        const uintptr_t End = Begin + MaxDistance;
        for (uintptr_t Site = FindRawCall(Begin, End); Site; Site = FindRawCall(Site + 1, End))
        {
            if (Finder::RelativeTarget(Site, 1, 5) == Target)
            {
                return true;
            }
        }
        return false;
    }

    void ResolveGUObjectArray()
    {
        const uintptr_t UObjectBaseInit = FunctionOf(L"Presizing for max %d objects, including %i objects not considered by GC, pre-allocating %i bytes for permanent pool.");
        const uintptr_t AllocatePool = FunctionOf(L"Max UObject count is invalid. It must be a number that is greater than 0.");
        const uintptr_t End = Finder::FunctionEnd(UObjectBaseInit);

        uintptr_t Array = 0;
        if (UObjectBaseInit && AllocatePool && End)
        {
            for (uintptr_t Lea = FindInstruction(UObjectBaseInit, End, { 0x48, 0x8D, 0x0D }); Lea; Lea = FindInstruction(Lea + 1, End, { 0x48, 0x8D, 0x0D }))
            {
                if (CallWithin(Lea, 0x40, AllocatePool))
                {
                    Array = Finder::RelativeTarget(Lea, 3, 7);
                    break;
                }
            }
        }
        RequireAddress(L"GUObjectArray", reinterpret_cast<uintptr_t&>(Natives::GUObjectArray), Array);
    }

    void ResolveNames()
    {
        uintptr_t AppendString = 0;
        uintptr_t GNames = 0;

        const uintptr_t Ref = First(Finder::StringRefs("ForwardShadingQuality_"));
        for (uintptr_t Site = FindRawCall(Ref, Ref + 0x80); Site; Site = FindRawCall(Site + 5, Ref + 0x80))
        {
            const uintptr_t Candidate = Finder::RelativeTarget(Site, 1, 5);
            const uintptr_t GetNames = Finder::NextCall(Candidate, 0x20);
            const uintptr_t Mov = GetNames ? FindInstruction(GetNames, GetNames + 0x20, { 0x48, 0x8B, 0x05 }) : 0;
            if (Mov)
            {
                AppendString = Candidate;
                GNames = Finder::RelativeTarget(Mov, 3, 7);
                break;
            }
        }

        Require(L"FName::AppendString", Natives::FName_AppendString, AppendString);
        RequireAddress(L"GNames", reinterpret_cast<uintptr_t&>(Natives::GNames), GNames);

        const uintptr_t LogInit = First(Finder::StringRefs(L"LogInit"));
        const uintptr_t CategoryCtor = Finder::NextCall(LogInit, 0x40);
        const uintptr_t Construct = Finder::NextCall(CategoryCtor, 0x40);
        Require(L"FName::FName", Natives::FName_Construct, Construct);
    }

    void ResolveObjectGlobals()
    {
        Require(L"StaticFindObject", Natives::StaticFindObject, FunctionOf(L"Illegal call to StaticFindObject() while serializing object data!"));
        Require(L"StaticLoadObject", Natives::StaticLoadObject, FunctionOf(L"Calling StaticLoadObject during PostLoad may result in hitches during streaming."));

        const uintptr_t StaticAllocate = FunctionOf(L"Objects have the same fully qualified name but different paths.\n\tNew Object: %s %s.%s\n\tExisting Object: %s");
        uintptr_t Construct = 0;
        const uintptr_t Ref = First(Finder::StringRefs(L"UGameplayStatics::SpawnObject null outer"));
        if (StaticAllocate && Ref)
        {
            const uintptr_t Function = Finder::FunctionStart(Ref);
            const uintptr_t End = Finder::FunctionEnd(Function);
            for (uintptr_t Site = FindRawCall(Function, End); Site; Site = FindRawCall(Site + 5, End))
            {
                const uintptr_t Target = Finder::RelativeTarget(Site, 1, 5);
                if (CallWithin(Target, 0x200, StaticAllocate))
                {
                    Construct = Target;
                }
            }
        }
        Require(L"StaticConstructObject_Internal", Natives::StaticConstructObject_Internal, Construct);
    }

    uintptr_t FindBackward(uintptr_t Reference, size_t MaxDistance, std::initializer_list<int32_t> Pattern)
    {
        const uintptr_t Begin = Reference - MaxDistance;
        uintptr_t Match = 0;
        for (uintptr_t Found = FindInstruction(Begin, Reference, Pattern); Found; Found = FindInstruction(Found + 1, Reference, Pattern))
        {
            Match = Found;
        }
        return Match;
    }

    void ResolveMemory()
    {
        uintptr_t GMalloc = 0;
        const uintptr_t PurgatoryRef = First(Finder::StringRefs(L"Purgatory proxy is now on."));
        if (PurgatoryRef)
        {
            for (uintptr_t Scan = PurgatoryRef - 0x200; Scan + 9 <= PurgatoryRef; ++Scan)
            {
                const uint8_t Rex = Byte(Scan + 1);
                if (Byte(Scan) == 0xF0 && (Rex == 0x48 || Rex == 0x49 || Rex == 0x4C || Rex == 0x4D) && Byte(Scan + 2) == 0x0F && Byte(Scan + 3) == 0xB1 && (Byte(Scan + 4) & 0xC7) == 0x05)
                {
                    GMalloc = Finder::RelativeTarget(Scan, 5, 9);
                }
            }
        }
        RequireAddress(L"GMalloc", reinterpret_cast<uintptr_t&>(Natives::GMalloc), GMalloc);

        const uintptr_t CollectInternal = FunctionOf(L"CollectGarbageInternal() is flushing async loading");
        const uintptr_t TryCollect = FunctionOf(L"TryCollectGarbage: forcing GC after %d skipped attempts.");
        uintptr_t Collect = 0;
        if (CollectInternal)
        {
            for (uintptr_t Site : Finder::CallsTo(CollectInternal, false))
            {
                const uintptr_t Function = Finder::FunctionStart(Site);
                if (Function && Function != TryCollect)
                {
                    Collect = Function;
                }
            }
        }
        Require(L"CollectGarbage", Natives::CollectGarbage, Collect);
    }

    std::vector<std::pair<uintptr_t, uintptr_t>> MovStores(uintptr_t Begin, uintptr_t End, std::initializer_list<int32_t> Prefix, uint32_t OperandOffset, uint32_t Length)
    {
        std::vector<std::pair<uintptr_t, uintptr_t>> Stores;
        for (uintptr_t At = FindInstruction(Begin, End, Prefix); At; At = FindInstruction(At + 1, End, Prefix))
        {
            Stores.emplace_back(At, Finder::RelativeTarget(At, OperandOffset, Length));
        }
        return Stores;
    }

    void ResolvePreInitGlobals()
    {
        const uintptr_t Ref = First(Finder::StringRefs(L"AllowCommandletRendering"));
        const uintptr_t PreInit = Finder::FunctionStart(Ref);
        const uintptr_t PreInitEnd = Finder::FunctionEnd(PreInit);
        if (!Ref || !PreInit || !PreInitEnd)
        {
            RequireAddress(L"FEngineLoop::PreInit", reinterpret_cast<uintptr_t&>(Natives::GIsClient), 0);
            return;
        }

        std::vector<std::pair<uintptr_t, uintptr_t>> Flags = MovStores(Ref - 0x40, Ref, { 0xC6, 0x05 }, 2, 7);
        Flags.erase(std::remove_if(Flags.begin(), Flags.end(), [](const auto& Store) { return Byte(Store.first + 6) != 0x01; }), Flags.end());
        std::sort(Flags.begin(), Flags.end());
        if (Flags.size() >= 2)
        {
            RequireAddress(L"GIsClient", reinterpret_cast<uintptr_t&>(Natives::GIsClient), Flags[0].second);
            RequireAddress(L"GIsServer", reinterpret_cast<uintptr_t&>(Natives::GIsServer), Flags[1].second);
        }
        else
        {
            RequireAddress(L"GIsClient", reinterpret_cast<uintptr_t&>(Natives::GIsClient), 0);
        }

        const uintptr_t ThreadIdSlot = Finder::ImportSlot("KERNEL32.dll", "GetCurrentThreadId");
        for (uintptr_t Call = FindInstruction(PreInit, PreInitEnd, { 0xFF, 0x15 }); Call; Call = FindInstruction(Call + 1, PreInitEnd, { 0xFF, 0x15 }))
        {
            if (Finder::RelativeTarget(Call, 2, 6) != ThreadIdSlot)
            {
                continue;
            }
            const uintptr_t InitByte = FindInstruction(Call, Call + 0x20, { 0xC6, 0x05 });
            const uintptr_t IdStore = FindInstruction(Call, Call + 0x20, { 0x89, 0x05 });
            if (InitByte)
            {
                RequireAddress(L"GIsGameThreadIdInitialized", reinterpret_cast<uintptr_t&>(Natives::GIsGameThreadIdInitialized), Finder::RelativeTarget(InitByte, 2, 7));
            }
            if (IdStore)
            {
                RequireAddress(L"GGameThreadId", reinterpret_cast<uintptr_t&>(Natives::GGameThreadId), Finder::RelativeTarget(IdStore, 2, 6));
            }
            break;
        }

        const uintptr_t LeaBefore = FindBackward(Ref, 0x10, { 0xE8 });
        if (LeaBefore)
        {
            Require(L"FCommandLine::Get", Natives::FCommandLine_Get, Finder::RelativeTarget(LeaBefore, 1, 5));
        }
        Require(L"FParse::Param", Natives::FParse_Param, Finder::NextCall(Ref, 0x20));
    }

    void ResolveEngineReadiness()
    {
        const uintptr_t Init = FunctionOf(L"Failed to load UnrealEd Engine class '%s'.");
        const uintptr_t InitEnd = Finder::FunctionEnd(Init);
        if (Init && InitEnd)
        {
            const std::vector<std::pair<uintptr_t, uintptr_t>> Stores = MovStores(Init, InitEnd, { 0x48, 0x89, 0x05 }, 3, 7);
            RequireAddress(L"GEngine", reinterpret_cast<uintptr_t&>(Natives::GEngine), Stores.empty() ? 0 : Stores.front().second);

            std::vector<uintptr_t> InitOnes;
            for (const auto& Store : MovStores(Init, InitEnd, { 0xC6, 0x05 }, 2, 7))
            {
                if (Byte(Store.first + 6) == 0x01)
                {
                    InitOnes.push_back(Store.second);
                }
            }

            const uintptr_t HandleError = FunctionOf(L"HandleError re-entered.");
            const uintptr_t HandleErrorEnd = Finder::FunctionEnd(HandleError);
            uintptr_t Running = 0;
            if (HandleError && HandleErrorEnd)
            {
                for (const auto& Store : MovStores(HandleError, HandleErrorEnd, { 0xC6, 0x05 }, 2, 7))
                {
                    if (Byte(Store.first + 6) != 0x00)
                    {
                        continue;
                    }
                    if (std::find(InitOnes.begin(), InitOnes.end(), Store.second) != InitOnes.end())
                    {
                        Running = Store.second;
                    }
                }
            }
            RequireAddress(L"GIsRunning", reinterpret_cast<uintptr_t&>(Natives::GIsRunning), Running);
        }
        else
        {
            RequireAddress(L"GEngine", reinterpret_cast<uintptr_t&>(Natives::GEngine), 0);
        }
    }

    void ResolveGameThreadTick()
    {
        const uintptr_t Tick = FunctionOf(L"Issuing initial cause event passed from URL: %s");
        Require(L"UGameEngine::Tick", Natives::UGameEngine_Tick, Tick);

        const uintptr_t TickEnd = Finder::FunctionEnd(Tick);
        if (Tick && TickEnd)
        {
            uintptr_t World = 0;
            for (const int32_t ModRm : { 0x05, 0x0D, 0x15, 0x1D, 0x25, 0x2D, 0x35, 0x3D })
            {
                const uintptr_t Store = FindInstruction(Tick, TickEnd, { 0x48, 0x89, ModRm });
                if (Store && (!World || Store < World))
                {
                    World = Finder::RelativeTarget(Store, 3, 7);
                }
            }
            RequireAddress(L"GWorld", reinterpret_cast<uintptr_t&>(Natives::GWorld), World);
        }
    }

    uintptr_t CallBeforeJccPastRef(uintptr_t Reference, size_t MaxBack)
    {
        for (uintptr_t Scan = Reference - 1; Scan >= Reference - MaxBack; --Scan)
        {
            if (Byte(Scan) != 0xE8 || Byte(Scan + 5) != 0x84 || Byte(Scan + 6) != 0xC0)
            {
                continue;
            }
            const uintptr_t Jcc = Scan + 7;
            uintptr_t JumpTarget = 0;
            if (Byte(Jcc) == 0x75)
            {
                JumpTarget = Jcc + 2 + static_cast<int8_t>(Byte(Jcc + 1));
            }
            else if (Byte(Jcc) == 0x0F && Byte(Jcc + 1) == 0x85)
            {
                JumpTarget = Jcc + 6 + *reinterpret_cast<const int32_t*>(Jcc + 2);
            }
            else
            {
                continue;
            }
            if (JumpTarget > Reference)
            {
                return Scan;
            }
        }
        return 0;
    }

    void ResolveEngineNet()
    {
        Require(L"UEngine::LoadMap", Natives::UEngine_LoadMap, FunctionOf(L"LoadMap: %s"));
        Require(L"UGameInstance::StartGameInstance", Natives::UGameInstance_StartGameInstance, FunctionOf(L"Failed to enter %s: %s. Please check the log for errors."));
        Require(L"GetSourceIniHierarchyFilenames", Natives::GetSourceIniHierarchyFilenames, FunctionOf(L"%sNotForLicensees/Default%s.ini"));

        const uintptr_t ListenLog = First(Finder::StringRefs(L"LoadMap: failed to Listen(%s)"));
        const uintptr_t ListenCall = ListenLog ? CallBeforeJccPastRef(ListenLog, 0x100) : 0;
        if (ListenCall)
        {
            Sites::LoadMapListenCall = reinterpret_cast<uint8_t*>(ListenCall);
        }
        else
        {
            Log::Error(L"Finder: LoadMap Listen call site not found");
            ++MissingCount;
        }

        uintptr_t Derive = 0;
        std::vector<uintptr_t> ListenFns;
        for (uintptr_t Ref : Finder::StringRefs(L"listen"))
        {
            ListenFns.push_back(Finder::FunctionStart(Ref));
        }
        for (uintptr_t Candidate : ListenFns)
        {
            if (Candidate && std::count(ListenFns.begin(), ListenFns.end(), Candidate) >= 2)
            {
                Derive = Candidate;
                break;
            }
        }
        Require(L"UWorld::AttemptDeriveFromURL", Natives::UWorld_AttemptDeriveFromURL, Derive);

        const uintptr_t RecordRef = First(Finder::StringRefs(L"RecordReplay: failed to create demo net driver!"));
        const uintptr_t CreateCall = RecordRef ? CallBeforeJccPastRef(RecordRef, 0x60) : 0;
        Require(L"UEngine::CreateNamedNetDriver", Natives::UEngine_CreateNamedNetDriver, CreateCall ? Finder::RelativeTarget(CreateCall, 1, 5) : 0);
    }

    void ResolvePreInitServerPatch()
    {
        const uintptr_t Ref = First(Finder::StringRefs(L"AllowCommandletRendering"));
        const uintptr_t PreInit = Finder::FunctionStart(Ref);
        const uintptr_t PreInitEnd = Finder::FunctionEnd(PreInit);
        const uintptr_t GIsClient = reinterpret_cast<uintptr_t>(Natives::GIsClient);
        if (!PreInit || !PreInitEnd || !GIsClient)
        {
            Log::Error(L"Finder: PreInit server-mode site prerequisites missing");
            ++MissingCount;
            return;
        }

        for (uintptr_t Site = FindInstruction(PreInit, PreInitEnd, { 0x41, 0xB5, 0x01, 0x40, 0x88, 0x35 }); Site; Site = FindInstruction(Site + 1, PreInitEnd, { 0x41, 0xB5, 0x01, 0x40, 0x88, 0x35 }))
        {
            const uintptr_t ClientStore = Site + 10;
            if (Byte(ClientStore) == 0x44 && Byte(ClientStore + 1) == 0x88 && Byte(ClientStore + 2) == 0x2D && Finder::RelativeTarget(ClientStore, 3, 7) == GIsClient)
            {
                Sites::PreInitRegularClient = reinterpret_cast<uint8_t*>(Site);
                return;
            }
        }

        Log::Error(L"Finder: PreInit regular-client block not found");
        ++MissingCount;
    }

    void ResolveLogging()
    {
        uintptr_t Logf = 0;
        for (const wchar_t* Anchor : { L"LoadMap: %s", L"UGameplayStatics::SpawnObject null outer" })
        {
            const uintptr_t Ref = First(Finder::StringRefs(Anchor));
            const uintptr_t Target = Finder::NextCall(Ref, 0x40);
            if (!Target)
            {
                continue;
            }
            if (Logf && Logf != Target)
            {
                Logf = 0;
                break;
            }
            Logf = Target;
        }
        Require(L"FMsg::Logf_Internal", Natives::FMsg_Logf_Internal, Logf);
    }
}

bool Finder::ResolveAll()
{
    MissingCount = 0;
    Memcury::Scanner::SetTargetModule(nullptr);

    ResolveGUObjectArray();
    ResolveNames();
    ResolveObjectGlobals();
    ResolveMemory();
    ResolveLogging();
    ResolvePreInitGlobals();
    ResolveEngineReadiness();
    ResolveGameThreadTick();

    Log::Info(L"Finder: static resolution complete, {} missing", MissingCount);
    return MissingCount == 0;
}
