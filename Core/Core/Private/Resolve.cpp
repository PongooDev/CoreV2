#include "pch.h"
#include "Finder.h"
#include "Natives.h"
#include "Log.h"
#include "ObjectLayout.h"
#include "Engine/EngineBaseTypes.h"
#include "Engine/World.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"

#include <algorithm>
#include <unordered_map>

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
        const uintptr_t PurgatoryFunction = Finder::FunctionStart(PurgatoryRef);
        const uintptr_t PurgatoryEnd = Finder::FunctionEnd(PurgatoryFunction);
        if (PurgatoryRef && PurgatoryFunction && PurgatoryEnd)
        {
            int32_t Exchanges = 0;
            for (uintptr_t Scan = PurgatoryFunction; Scan + 9 <= PurgatoryEnd; ++Scan)
            {
                const uint8_t Rex = Byte(Scan + 1);
                if (Byte(Scan) == 0xF0 && (Rex == 0x48 || Rex == 0x49 || Rex == 0x4C || Rex == 0x4D) && Byte(Scan + 2) == 0x0F && Byte(Scan + 3) == 0xB1 && (Byte(Scan + 4) & 0xC7) == 0x05)
                {
                    GMalloc = Finder::RelativeTarget(Scan, 5, 9);
                    ++Exchanges;
                }
            }
            if (Exchanges != 1)
            {
                Log::Error(L"Finder: expected 1 GMalloc exchange, found {}", Exchanges);
                GMalloc = 0;
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

    uintptr_t FirstCall(uintptr_t Function)
    {
        const uintptr_t End = Finder::FunctionEnd(Function);
        if (!Function || !End)
        {
            return 0;
        }
        const std::vector<std::pair<uintptr_t, uintptr_t>> Calls = Finder::DirectCalls(Function, End);
        return Calls.empty() ? 0 : Calls.front().second;
    }

    bool IsInText(uintptr_t Address)
    {
        return Address >= Finder::TextBegin() && Address < Finder::TextEnd();
    }

    bool IsListenCall(uintptr_t Call)
    {
        const uintptr_t World = Call - 10;
        const bool bWorld = (Byte(World) == 0x48 || Byte(World) == 0x49) && Byte(World + 1) == 0x8B && (Byte(World + 2) & 0xF8) == 0x88 && (Byte(World + 2) & 0x07) != 0x04;

        const uintptr_t Url = Call - 3;
        const bool bUrl = (Byte(Url) == 0x48 || Byte(Url) == 0x49) && Byte(Url + 1) == 0x8B && (Byte(Url + 2) & 0xF8) == 0xD0;

        bool bOption = false;
        for (uintptr_t Lea = FindInstruction(Call - 0x30, Call, { 0x48, 0x8D, 0x15 }); Lea && !bOption; Lea = FindInstruction(Lea + 1, Call, { 0x48, 0x8D, 0x15 }))
        {
            const auto* Text = reinterpret_cast<const wchar_t*>(Finder::RelativeTarget(Lea, 3, 7));
            bOption = Finder::IsReadable(Text, sizeof(L"Listen")) && wcscmp(Text, L"Listen") == 0;
        }

        const bool bTarget = IsInText(Finder::RelativeTarget(Call, 1, 5));
        if (!bWorld || !bUrl || !bOption || !bTarget)
        {
            Log::Error(L"Finder: Listen call site checks failed (world {}, url {}, option {}, target {})", bWorld, bUrl, bOption, bTarget);
            return false;
        }
        return true;
    }

    constexpr int32_t WorldNetDriver = 0x38;
    constexpr int32_t WorldDemoNetDriver = 0xB8;

    uint8_t BaseRegister(uint8_t Rex, uint8_t ModRm)
    {
        return static_cast<uint8_t>(((Rex & 0x01) << 3) | (ModRm & 0x07));
    }

    bool FindInlinedNetMode(uintptr_t Call, uintptr_t GetNetMode, Sites::FInlinedNetMode& Out)
    {
        uintptr_t DemoLoad = 0;
        for (uintptr_t Scan = Call - 0x40; Scan + 7 <= Call; ++Scan)
        {
            if ((Byte(Scan) & 0xF8) == 0x48 && Byte(Scan + 1) == 0x8B && (Byte(Scan + 2) & 0xC0) == 0x80 && (Byte(Scan + 2) & 0x07) != 0x04 && *reinterpret_cast<const int32_t*>(Scan + 3) == WorldDemoNetDriver)
            {
                DemoLoad = Scan;
            }
        }
        if (!DemoLoad)
        {
            return false;
        }

        uintptr_t Jcc = 0;
        uint8_t TestBase = 0xFF;
        for (uintptr_t Scan = DemoLoad - 0x18; Scan < DemoLoad; ++Scan)
        {
            const uint8_t Rex = Byte(Scan);
            const uint8_t ModRm = Byte(Scan + 2);
            if ((Rex & 0xF8) != 0x48 || (ModRm & 0xC0) != 0x40 || Byte(Scan + 3) != WorldNetDriver)
            {
                continue;
            }
            if (Byte(Scan + 1) == 0x83 && (ModRm & 0x38) == 0x38 && Byte(Scan + 4) == 0x00)
            {
                Jcc = Scan + 5;
                TestBase = BaseRegister(Rex, ModRm);
            }
            else if (Byte(Scan + 1) == 0x39)
            {
                Jcc = Scan + 4;
                TestBase = BaseRegister(Rex, ModRm);
            }
        }
        if (!Jcc || TestBase != BaseRegister(Byte(DemoLoad), Byte(DemoLoad + 2)))
        {
            return false;
        }

        const bool bTaken = Byte(Jcc) == 0x74 && Jcc + 2 + static_cast<int8_t>(Byte(Jcc + 1)) == DemoLoad;
        const bool bFallThrough = Byte(Jcc) == 0x75 && Jcc + 2 == DemoLoad;
        if (!bTaken && !bFallThrough)
        {
            return false;
        }

        if (!CallWithin(DemoLoad + 7, Call - (DemoLoad + 7), GetNetMode))
        {
            return false;
        }

        Out.Branch = reinterpret_cast<uint8_t*>(Jcc);
        Out.DemoNetDriverDisplacement = reinterpret_cast<uint8_t*>(DemoLoad + 3);
        return true;
    }

    void ResolveEngineNet()
    {
        Require(L"UEngine::LoadMap", Natives::UEngine_LoadMap, FunctionOf(L"LoadMap: %s"));
        Require(L"UGameInstance::StartGameInstance", Natives::UGameInstance_StartGameInstance, FunctionOf(L"Failed to enter %s: %s. Please check the log for errors."));
        Require(L"GetSourceIniHierarchyFilenames", Natives::GetSourceIniHierarchyFilenames, FunctionOf(L"%sNotForLicensees/Default%s.ini"));

        const uintptr_t ListenLog = OnlyRef(L"LoadMap: failed to Listen(%s)");
        const uintptr_t ListenCall = ListenLog ? CallBeforeJccPastRef(ListenLog, 0x100) : 0;
        if (ListenCall && IsListenCall(ListenCall))
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

        uintptr_t FindNamed = 0;
        uintptr_t SetWorld = 0;
        uintptr_t FindCollection = 0;
        if (CreateCall)
        {
            uintptr_t Jcc = CreateCall + 7;
            uintptr_t JumpTarget = 0;
            if (Byte(Jcc) == 0x75)
            {
                JumpTarget = Jcc + 2 + static_cast<int8_t>(Byte(Jcc + 1));
            }
            else if (Byte(Jcc) == 0x0F && Byte(Jcc + 1) == 0x85)
            {
                JumpTarget = Jcc + 6 + *reinterpret_cast<const int32_t*>(Jcc + 2);
            }

            if (JumpTarget)
            {
                const uintptr_t FindCall = FindRawCall(JumpTarget, JumpTarget + 0x30);
                FindNamed = FindCall ? Finder::RelativeTarget(FindCall, 1, 5) : 0;

                const uintptr_t DemoStore = FindNamed ? FindInstruction(FindCall, FindCall + 0x40, { -1, 0x89, -1, 0xB8, 0x00, 0x00, 0x00 }) : 0;
                if (DemoStore && (Byte(DemoStore) & 0xF8) == 0x48)
                {
                    const uintptr_t SetCall = FindRawCall(DemoStore + 7, DemoStore + 0x20);
                    SetWorld = SetCall ? Finder::RelativeTarget(SetCall, 1, 5) : 0;

                    for (uintptr_t Call = SetCall ? FindRawCall(SetCall + 5, SetCall + 0x30) : 0; Call; Call = FindRawCall(Call + 1, SetCall + 0x30))
                    {
                        if (FindInstruction(Call - 8, Call, { 0x33, 0xD2 }))
                        {
                            FindCollection = Finder::RelativeTarget(Call, 1, 5);
                            break;
                        }
                    }
                }
            }
        }
        Require(L"UEngine::FindNamedNetDriver", Natives::UEngine_FindNamedNetDriver, FindNamed);
        Require(L"UNetDriver::SetWorld", Natives::UNetDriver_SetWorld, SetWorld);
        Require(L"UWorld::FindCollectionByType", Natives::UWorld_FindCollectionByType, FindCollection);

        uintptr_t Notify = 0;
        const uintptr_t SetWorldEnd = Finder::FunctionEnd(SetWorld);
        for (uintptr_t Lea = SetWorld && SetWorldEnd ? FindInstruction(SetWorld, SetWorldEnd, { 0x48, 0x8D, 0x47 }) : 0; Lea; Lea = FindInstruction(Lea + 1, SetWorldEnd, { 0x48, 0x8D, 0x47 }))
        {
            if (FindInstruction(Lea + 4, Lea + 0x14, { 0x48, 0x89, 0x83, 0xE8, 0x00, 0x00, 0x00 }))
            {
                Notify = Byte(Lea + 3);
                break;
            }
        }
        const uintptr_t NotifyBase = 0x10000;
        const uintptr_t CoreNotify = reinterpret_cast<uintptr_t>(static_cast<FNetworkNotify*>(reinterpret_cast<UWorld*>(NotifyBase))) - NotifyBase;
        if (!Notify || Notify != CoreNotify)
        {
            Log::Error(L"Finder: UWorld FNetworkNotify base is 0x{:X} in the game, 0x{:X} in Core", Notify, CoreNotify);
            ++MissingCount;
        }
    }

    void ResolveNetMode()
    {
        const uintptr_t Derive = reinterpret_cast<uintptr_t>(Natives::UWorld_AttemptDeriveFromURL);
        if (!Derive)
        {
            return;
        }

        std::vector<uintptr_t> Jumps;
        std::vector<uintptr_t> Calls;
        for (uintptr_t Site : Finder::CallsTo(Derive, true))
        {
            (Byte(Site) == 0xE9 ? Jumps : Calls).push_back(Site);
        }

        uintptr_t InternalGetNetMode = 0;
        uintptr_t GetNetMode = 0;
        if (Jumps.size() == 1)
        {
            for (uintptr_t Start = Jumps[0] & ~static_cast<uintptr_t>(0xF); Start + 0x40 > Jumps[0]; Start -= 0x10)
            {
                if (Byte(Start - 1) == 0xCC && Byte(Start) == 0x48 && Byte(Start + 1) == 0x83 && Byte(Start + 2) == 0x79 && Byte(Start + 4) == 0x00)
                {
                    InternalGetNetMode = Start;
                    break;
                }
            }

            const uintptr_t FirstJump = InternalGetNetMode ? FindInstruction(InternalGetNetMode, Jumps[0], { 0xE9 }) : 0;
            const uintptr_t Target = FirstJump ? Finder::RelativeTarget(FirstJump, 1, 5) : 0;
            if (Target >= Finder::TextBegin() && Target < Finder::TextEnd())
            {
                GetNetMode = Target;
            }
        }
        Require(L"UWorld::InternalGetNetMode", Natives::UWorld_InternalGetNetMode, InternalGetNetMode);
        Require(L"UNetDriver::GetNetMode", Natives::UNetDriver_GetNetMode, GetNetMode);

        int32_t Found = 0;
        for (uintptr_t Site : Calls)
        {
            Sites::FInlinedNetMode NetMode;
            if (GetNetMode && Found < static_cast<int32_t>(std::size(Sites::InlinedNetMode)) && FindInlinedNetMode(Site, GetNetMode, NetMode))
            {
                Sites::InlinedNetMode[Found++] = NetMode;
            }
        }
        if (Calls.size() != std::size(Sites::InlinedNetMode) || Found != static_cast<int32_t>(Calls.size()))
        {
            Log::Error(L"Finder: inlined net mode copies: {} calls, {} matched", Calls.size(), Found);
            std::fill(std::begin(Sites::InlinedNetMode), std::end(Sites::InlinedNetMode), Sites::FInlinedNetMode{});
            ++MissingCount;
        }
    }

    void ResolveWorldTravel()
    {
        const uintptr_t Travel = FunctionOf(L"Server switch level: %s");
        const uintptr_t TravelEnd = Finder::FunctionEnd(Travel);
        if (!Travel || !TravelEnd)
        {
            Require(L"UEngine::TickWorldTravel", Natives::FOnNetworkFailure_Broadcast, 0);
            return;
        }

        int32_t Countdowns = 0;
        for (uintptr_t Load = FindInstruction(Travel, TravelEnd, { 0xF3, 0x0F, 0x10 }); Load; Load = FindInstruction(Load + 1, TravelEnd, { 0xF3, 0x0F, 0x10 }))
        {
            const uint8_t ModRm = Byte(Load + 3);
            const int32_t Displacement = *reinterpret_cast<const int32_t*>(Load + 4);
            const uintptr_t Store = Load + 12;
            if ((ModRm & 0xC0) == 0x80 && Byte(Load + 8) == 0xF3 && Byte(Load + 9) == 0x0F && Byte(Load + 10) == 0x5C
                && Byte(Store) == 0xF3 && Byte(Store + 1) == 0x0F && Byte(Store + 2) == 0x11 && Byte(Store + 3) == ModRm && *reinterpret_cast<const int32_t*>(Store + 4) == Displacement)
            {
                Natives::UWorld_NextSwitchCountdown = Displacement;
                ++Countdowns;
            }
        }
        if (Countdowns != 1)
        {
            Log::Error(L"Finder: expected 1 NextSwitchCountdown update, found {}", Countdowns);
            Natives::UWorld_NextSwitchCountdown = -1;
            ++MissingCount;
        }

        uintptr_t Broadcast = 0;
        for (uintptr_t Type = FindInstruction(Travel, TravelEnd, { 0x41, 0xB9, ENetworkFailure::PendingConnectionFailure, 0x00, 0x00, 0x00 }); Type; Type = FindInstruction(Type + 1, TravelEnd, { 0x41, 0xB9, ENetworkFailure::PendingConnectionFailure, 0x00, 0x00, 0x00 }))
        {
            const uintptr_t Call = FindRawCall(Type + 6, Type + 0x10);
            for (uintptr_t Lea = Type - 0x18; Call && Lea < Type; ++Lea)
            {
                if ((Byte(Lea) & 0xFC) == 0x48 && Byte(Lea + 1) == 0x8D && (Byte(Lea + 2) & 0xF8) == 0x88 && (Byte(Lea + 2) & 0x07) != 0x04)
                {
                    Natives::UEngine_NetworkFailureEvent = *reinterpret_cast<const int32_t*>(Lea + 3);
                    Broadcast = Finder::RelativeTarget(Call, 1, 5);
                }
            }
            if (Broadcast)
            {
                break;
            }
        }

        int32_t Agreeing = 0;
        for (uintptr_t Site : Broadcast ? Finder::CallsTo(Broadcast, false) : std::vector<uintptr_t>())
        {
            const uintptr_t Add = FindInstruction(Site - 0x20, Site, { 0x48, 0x81, 0xC1 });
            if (Add && *reinterpret_cast<const int32_t*>(Add + 3) == Natives::UEngine_NetworkFailureEvent)
            {
                ++Agreeing;
            }
        }
        if (Agreeing == 0)
        {
            Broadcast = 0;
            Natives::UEngine_NetworkFailureEvent = -1;
        }
        Require(L"FOnNetworkFailure::Broadcast", Natives::FOnNetworkFailure_Broadcast, Broadcast);
    }

    void ResolveGameMapsSettings()
    {
        const uintptr_t Start = reinterpret_cast<uintptr_t>(Natives::UGameInstance_StartGameInstance);
        const std::vector<std::pair<uintptr_t, uintptr_t>> StartCalls = Finder::DirectCalls(Start, Finder::FunctionEnd(Start));

        uintptr_t StaticClass = 0;
        uintptr_t GameDefaultMap = 0;
        int32_t Matches = 0;
        for (const auto& [Site, Target] : StartCalls)
        {
            const uintptr_t TargetFirst = FirstCall(Target);
            const bool bAfterStaticClass = TargetFirst && std::any_of(StartCalls.begin(), StartCalls.end(), [&](const auto& Other)
            {
                return Other.first < Site && Other.first + 0x40 >= Site && Other.second == TargetFirst;
            });
            if (bAfterStaticClass && Target != GameDefaultMap)
            {
                StaticClass = TargetFirst;
                GameDefaultMap = Target;
                ++Matches;
            }
        }
        if (Matches != 1)
        {
            GameDefaultMap = 0;
            StaticClass = 0;
        }
        Require(L"UGameMapsSettings::GetGameDefaultMap", Natives::UGameMapsSettings_GetGameDefaultMap, GameDefaultMap);

        uintptr_t GlobalDefaultGameMode = 0;
        const uintptr_t PrefixRef = OnlyRef(L"Failed to load game mode '%s' specified by prefixed map name %s.");
        const uintptr_t CreateGameMode = Finder::FunctionStart(PrefixRef);
        Matches = 0;
        for (const auto& [Site, Target] : StaticClass && CreateGameMode ? Finder::DirectCalls(PrefixRef, Finder::FunctionEnd(CreateGameMode)) : std::vector<std::pair<uintptr_t, uintptr_t>>())
        {
            if (Target != GlobalDefaultGameMode && FirstCall(Target) == StaticClass)
            {
                GlobalDefaultGameMode = Target;
                ++Matches;
            }
        }
        if (Matches != 1)
        {
            GlobalDefaultGameMode = 0;
        }
        Require(L"UGameMapsSettings::GetGlobalDefaultGameMode", Natives::UGameMapsSettings_GetGlobalDefaultGameMode, GlobalDefaultGameMode);
    }

    void ResolveCreateDefaultObject()
    {
        const int32_t DefaultObject = ObjectLayout::Current.UClass_ClassDefaultObject;
        const uintptr_t Begin = Finder::TextBegin();
        const uintptr_t End = Finder::TextEnd();

        std::unordered_map<int32_t, int32_t> Votes;
        int32_t Total = 0;
        for (uintptr_t At = Begin; At + 8 <= End; ++At)
        {
            if ((Byte(At) & 0xF8) != 0x48 || (Byte(At + 2) & 0xC0) != 0x80 || (Byte(At + 2) & 0x07) == 0x04 || *reinterpret_cast<const int32_t*>(At + 3) != DefaultObject)
            {
                continue;
            }

            size_t Length = 0;
            if (Byte(At + 1) == 0x39)
            {
                Length = 7;
            }
            else if (Byte(At + 1) == 0x83 && (Byte(At + 2) & 0x38) == 0x38 && Byte(At + 7) == 0x00)
            {
                Length = 8;
            }
            if (!Length)
            {
                continue;
            }

            for (uintptr_t Call = At + Length; Call + 6 <= At + Length + 0x18 && Call + 6 <= End; ++Call)
            {
                if (Byte(Call) == 0xFF && (Byte(Call + 1) & 0xF8) == 0x90 && (Byte(Call + 1) & 0x07) != 0x04)
                {
                    ++Votes[*reinterpret_cast<const int32_t*>(Call + 2)];
                    ++Total;
                    break;
                }
            }
        }

        int32_t Winner = -1;
        int32_t WinnerVotes = 0;
        for (const auto& [Displacement, Count] : Votes)
        {
            if (Count > WinnerVotes)
            {
                Winner = Displacement;
                WinnerVotes = Count;
            }
        }

        if (Winner > 0 && Winner % 8 == 0 && WinnerVotes * 10 >= Total * 9)
        {
            Natives::UClass_CreateDefaultObjectIndex = Winner / 8;
            Log::Info(L"Finder: UClass::CreateDefaultObject is slot 0x{:X} ({} of {} sites)", Winner / 8, WinnerVotes, Total);
        }
        else
        {
            Log::Error(L"Finder: UClass::CreateDefaultObject vote failed ({} of {} sites)", WinnerVotes, Total);
            ++MissingCount;
        }
    }

    void ResolveRequestingExit()
    {
        uintptr_t RequestingExit = 0;
        bool bAgree = true;
        const std::vector<uintptr_t> Refs = Finder::StringRefs(L"FPlatformMisc::RequestExit(%i)");
        for (uintptr_t Ref : Refs)
        {
            const uintptr_t Function = Finder::FunctionStart(Ref);
            std::vector<std::pair<uintptr_t, uintptr_t>> Stores = MovStores(Function, Finder::FunctionEnd(Function), { 0xC6, 0x05 }, 2, 7);
            Stores.erase(std::remove_if(Stores.begin(), Stores.end(), [](const auto& Store) { return Byte(Store.first + 6) != 0x01; }), Stores.end());
            if (Stores.size() != 1 || (RequestingExit && RequestingExit != Stores.front().second))
            {
                bAgree = false;
                break;
            }
            RequestingExit = Stores.front().second;
        }
        RequireAddress(L"GIsRequestingExit", reinterpret_cast<uintptr_t&>(Natives::GIsRequestingExit), bAgree && Refs.size() >= 2 ? RequestingExit : 0);
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
    ResolveRequestingExit();
    ResolveEngineNet();
    ResolveNetMode();
    ResolveWorldTravel();
    ResolveGameMapsSettings();
    ResolveCreateDefaultObject();
    ResolvePreInitServerPatch();

    Log::Info(L"Finder: static resolution complete, {} missing", MissingCount);
    return MissingCount == 0;
}

namespace
{
    void** CdoVTable(const wchar_t* ClassPath)
    {
        UClass* Class = FindObject<UClass>(nullptr, ClassPath);
        UObject* Default = Class ? Class->GetDefaultObject() : nullptr;
        return Default ? *reinterpret_cast<void***>(Default) : nullptr;
    }
}

bool Finder::ResolveProcessEvent()
{
    if (void** ObjectVTable = CdoVTable(L"/Script/CoreUObject.Object"))
    {
        for (int32_t Index = 0; Index < 0x80; ++Index)
        {
            const uintptr_t Function = reinterpret_cast<uintptr_t>(ObjectVTable[Index]);
            if (!IsReadable(reinterpret_cast<void*>(Function), 0x200))
            {
                break;
            }
            if (FindInstruction(Function, Function + 0x200, { 0xF7, -1, 0x88, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00 }))
            {
                Natives::ProcessEventIndex = Index;
                break;
            }
        }
    }

    if (Natives::ProcessEventIndex < 0)
    {
        Log::Error(L"Finder: ProcessEvent index not found");
        return false;
    }
    Log::Info(L"Finder: ProcessEvent is slot 0x{:X}", Natives::ProcessEventIndex);
    return true;
}

bool Finder::ResolveInitListen()
{
    const uintptr_t InitListen = FunctionOf(L"UNetDriver::InitListen");
    if (void** NetDriverVTable = CdoVTable(L"/Script/Engine.NetDriver"))
    {
        Natives::UNetDriver_InitListenIndex = VTableIndex(NetDriverVTable, reinterpret_cast<void*>(InitListen), 0x6A);
    }

    if (Natives::UNetDriver_InitListenIndex < 0)
    {
        Log::Error(L"Finder: UNetDriver::InitListen index not found");
        return false;
    }
    Log::Info(L"Finder: UNetDriver::InitListen is slot 0x{:X}", Natives::UNetDriver_InitListenIndex);
    return true;
}
