#include "pch.h"
#include "AbilitySystemComponent.h"
#include "Finder.h"
#include "Natives.h"
#include "Log.h"
#include "ObjectLayout.h"
#include "Engine/EngineBaseTypes.h"
#include "AI/Navigation/NavigationSystem.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "Net/DataBunch.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"

#include <algorithm>
#include <unordered_map>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace
{
    int32_t MissingCount = 0;

    uintptr_t First(const std::vector<uintptr_t>& Items)
    {
        return Items.empty() ? 0 : Items.front();
    }

    template <typename CharType>
    uintptr_t OnlyRef(const CharType* Text)
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

    bool IsInText(uintptr_t Address)
    {
        return Address >= Finder::TextBegin() && Address < Finder::TextEnd();
    }

    // A vtable slot Core has swapped holds one of this module's functions.
    bool IsCoreFunction(uintptr_t Address)
    {
        const uintptr_t Base = reinterpret_cast<uintptr_t>(&__ImageBase);
        const IMAGE_NT_HEADERS* Headers = reinterpret_cast<const IMAGE_NT_HEADERS*>(Base + __ImageBase.e_lfanew);
        return Address >= Base && Address < Base + Headers->OptionalHeader.SizeOfImage;
    }

    uintptr_t FindRawCall(uintptr_t Begin, uintptr_t End)
    {
        for (uintptr_t Address = Begin; Address + 5 <= End; ++Address)
        {
            if (Byte(Address) == 0xE8 && IsInText(Finder::RelativeTarget(Address, 1, 5)))
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
        Require(L"FLogCategoryBase::FLogCategoryBase", Natives::FLogCategoryBase_FLogCategoryBase, CategoryCtor);
    }

    void ResolveObjectGlobals()
    {
        Require(L"StaticFindObject", Natives::StaticFindObject, FunctionOf(L"Illegal call to StaticFindObject() while serializing object data!"));
        Require(L"StaticLoadObject", Natives::StaticLoadObject, FunctionOf(L"Calling StaticLoadObject during PostLoad may result in hitches during streaming."));

        const uintptr_t StaticAllocate = FunctionOf(L"Objects have the same fully qualified name but different paths.\n\tNew Object: %s %s.%s\n\tExisting Object: %s");
        uintptr_t Construct = 0;
        const uintptr_t Ref = First(Finder::StringRefs(L"NewObject with empty name can't be used to create default subobjects (inside of UObject derived class constructor) as it produces inconsistent object names. Use ObjectInitializer.CreateDefaultSuobject<> instead."));
        if (StaticAllocate && Ref)
        {
            for (uintptr_t Site = FindRawCall(Ref, Ref + 0x80); Site && !Construct; Site = FindRawCall(Site + 5, Ref + 0x80))
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
        if (Flags.size() >= 3)
        {
            RequireAddress(L"GIsClient", reinterpret_cast<uintptr_t&>(Natives::GIsClient), Flags[0].second);
            RequireAddress(L"GIsServer", reinterpret_cast<uintptr_t&>(Natives::GIsServer), Flags[1].second);
            RequireAddress(L"PRIVATE_GIsRunningCommandlet", reinterpret_cast<uintptr_t&>(Natives::PRIVATE_GIsRunningCommandlet), Flags[2].second);
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
        Out.NetDriverDisplacement = static_cast<uint8_t>(WorldNetDriver);
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
            // The store into UNetDriver::Notify: +0xE8 on 1.1 and 1.2, +0xF0 on 1.7.2 (its new WorldPackage at +0xA8).
            if (FindInstruction(Lea + 4, Lea + 0x20, { 0x48, 0x89, 0x83, -1, -1, 0x00, 0x00 }))
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

    void ResolveCoreUtilities()
    {
        const uintptr_t RandSlot = Finder::ImportSlot("api-ms-win-crt-utility-l1-1-0.dll", "rand");
        Require(L"rand", Natives::CRT_rand, RandSlot ? *reinterpret_cast<const uintptr_t*>(RandSlot) : 0);

        uintptr_t SRand = 0;
        int32_t Matches = 0;
        for (uintptr_t Site = FindInstruction(Finder::TextBegin(), Finder::TextEnd(), { 0x69, 0x0D, -1, -1, -1, -1, 0x35, 0x84, 0xB3, 0x0B }); Site; Site = FindInstruction(Site + 1, Finder::TextEnd(), { 0x69, 0x0D, -1, -1, -1, -1, 0x35, 0x84, 0xB3, 0x0B }))
        {
            const uintptr_t Start = Finder::FunctionStart(Site);
            const bool bFunctionStart = Start ? Start == Site : Byte(Site - 1) == 0xCC;
            if (bFunctionStart && FindInstruction(Site + 10, Site + 0x20, { 0x81, 0xC1, 0x6B, 0x63, 0x19, 0x36 }))
            {
                SRand = Site;
                ++Matches;
            }
        }
        Require(L"FMath::SRand", Natives::FMath_SRand, Matches == 1 ? SRand : 0);

        Require(L"FUObjectArray::AllocateSerialNumber", Natives::FUObjectArray_AllocateSerialNumber, FunctionOf(L"UObject serial numbers overflowed (trying to allocate serial number %d)."));
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
                break;
            }
        }
        if (!Sites::PreInitRegularClient)
        {
            Log::Error(L"Finder: PreInit regular-client block not found");
            ++MissingCount;
        }
    }

    std::vector<uint8_t*> CommandletChecks(uintptr_t Function)
    {
        std::vector<uint8_t*> Checks;
        const uintptr_t End = Finder::FunctionEnd(Function);
        const uintptr_t Commandlet = reinterpret_cast<uintptr_t>(Natives::PRIVATE_GIsRunningCommandlet);
        for (uintptr_t Compare = Function; Function && End && Compare + 6 <= End; ++Compare)
        {
            if (Byte(Compare) != 0x38 || (Byte(Compare + 1) & 0xC7) != 0x05 || Finder::RelativeTarget(Compare, 2, 6) != Commandlet)
            {
                continue;
            }
            for (uintptr_t Jump = Compare + 6; Jump < Compare + 0x12; ++Jump)
            {
                if (Byte(Jump) == 0x75 || (Byte(Jump) == 0x0F && Byte(Jump + 1) == 0x85))
                {
                    Checks.push_back(reinterpret_cast<uint8_t*>(Jump));
                    break;
                }
            }
        }
        return Checks;
    }

    void RequireChecks(const wchar_t* Name, std::vector<uint8_t*>& Destination, std::vector<uint8_t*> Checks, size_t Expected)
    {
        if (Checks.size() != Expected)
        {
            Log::Error(L"Finder: {} has {} dedicated-server checks, expected {}", Name, Checks.size(), Expected);
            ++MissingCount;
            return;
        }
        Destination = std::move(Checks);
    }

    void ResolveDedicatedServerChecks()
    {
        const uintptr_t PreInit = FunctionOf(L"AllowCommandletRendering");
        std::vector<uint8_t*> PreInitChecks = CommandletChecks(PreInit);
        if (Sites::PreInitRegularClient)
        {
            const uintptr_t PreInitEnd = Finder::FunctionEnd(PreInit);
            for (uintptr_t Test = FindInstruction(reinterpret_cast<uintptr_t>(Sites::PreInitRegularClient), PreInitEnd, { 0x45, 0x84, 0xED, 0x0F, 0x84 }); Test; Test = FindInstruction(Test + 1, PreInitEnd, { 0x45, 0x84, 0xED, 0x0F, 0x84 }))
            {
                PreInitChecks.push_back(reinterpret_cast<uint8_t*>(Test + 3));
            }
        }
        RequireChecks(L"FEngineLoop::PreInit", Sites::PreInitDedicatedServerChecks, std::move(PreInitChecks), 5);
        RequireChecks(L"UEngine::Init", Sites::EngineInitDedicatedServerChecks, CommandletChecks(FunctionOf(L"Initializing Engine...")), 2);
        RequireChecks(L"UGameEngine::Tick", Sites::GameEngineTickDedicatedServerChecks, CommandletChecks(reinterpret_cast<uintptr_t>(Natives::UGameEngine_Tick)), 2);
    }

    void ResolveOutputDevices()
    {
        uintptr_t LogSingleton = 0;
        uintptr_t AddOutputDevice = 0;
        const uintptr_t Ref = OnlyRef(L"AllowStdOutLogVerbosity");
        if (const uintptr_t Tail = Ref ? FindInstruction(Ref, Ref + 0x100, { 0xE8, -1, -1, -1, -1, 0x48, 0x8B, 0xC8, 0x48, 0x8B, 0xD3, 0x48, 0x83, 0xC4, -1, 0x5B, 0xE9 }) : 0)
        {
            LogSingleton = Finder::RelativeTarget(Tail, 1, 5);
            AddOutputDevice = Finder::RelativeTarget(Tail + 16, 1, 5);
        }
        Require(L"GetGlobalLogSingleton", Natives::GetGlobalLogSingleton, LogSingleton);
        Require(L"FOutputDeviceRedirector::AddOutputDevice", Natives::FOutputDeviceRedirector_AddOutputDevice, AddOutputDevice);
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
        Require(L"FDebug::AssertFailed", Natives::FDebug_AssertFailed, FunctionOf(L"Assertion failed: %s"));
        Require(L"LowLevelFatalErrorHandler", Natives::LowLevelFatalErrorHandler, FunctionOf(L"LowLevelFatalError"));
    }
}

bool Finder::ResolveRuntime()
{
    MissingCount = 0;
    Memcury::Scanner::SetTargetModule(nullptr);

    ResolveGUObjectArray();
    ResolveNames();
    ResolveObjectGlobals();
    ResolveMemory();
    ResolveLogging();
    ResolveOutputDevices();
    ResolveCoreUtilities();
    ResolvePreInitGlobals();
    ResolveEngineReadiness();
    ResolveGameThreadTick();
    ResolveRequestingExit();
    ResolveCreateDefaultObject();

    Log::Info(L"Finder: runtime resolved, {} missing", MissingCount);
    return MissingCount == 0;
}

namespace
{
    void** VTableStoredAfter(uintptr_t From, size_t MaxDistance)
    {
        for (uintptr_t Lea = FindInstruction(From, From + MaxDistance, { 0x48, 0x8D, 0x05 }); Lea; Lea = FindInstruction(Lea + 1, From + MaxDistance, { 0x48, 0x8D, 0x05 }))
        {
            const uint8_t ModRm = Byte(Lea + 9);
            if (Byte(Lea + 7) == 0x48 && Byte(Lea + 8) == 0x89 && (ModRm & 0xF8) == 0x00 && (ModRm & 0x07) != 0x04 && (ModRm & 0x07) != 0x05)
            {
                return reinterpret_cast<void**>(Finder::RelativeTarget(Lea, 3, 7));
            }
        }
        return nullptr;
    }

    void CollectDerivedVTables(uintptr_t Constructor, std::vector<void**>& VTables, int32_t Depth)
    {
        for (const uintptr_t Site : Finder::CallsTo(Constructor, false))
        {
            void** VTable = VTableStoredAfter(Site + 5, 0x10);
            if (!VTable || std::find(VTables.begin(), VTables.end(), VTable) != VTables.end())
            {
                continue;
            }
            VTables.push_back(VTable);
            if (Depth < 8)
            {
                CollectDerivedVTables(Finder::FunctionStart(Site), VTables, Depth + 1);
            }
        }
    }

    void ResolveConsole()
    {
        const uintptr_t Constructor = Finder::FunctionStart(OnlyRef(L"/Engine/EngineResources/Black"));
        const uintptr_t SuperCall = Constructor ? FindRawCall(Constructor, Constructor + 0x40) : 0;
        void** ConsoleVTable = SuperCall ? VTableStoredAfter(SuperCall + 5, 0x20) : nullptr;
        if (!ConsoleVTable)
        {
            Log::Error(L"Finder: UConsole vtable not found");
            ++MissingCount;
            return;
        }

        Sites::ConsoleVTables = { ConsoleVTable };
        CollectDerivedVTables(Constructor, Sites::ConsoleVTables, 0);

        for (int32_t Index = 0x43; Index < 0x60 && Natives::UConsole_BuildRuntimeAutoCompleteListIndex < 0; ++Index)
        {
            const uintptr_t Function = reinterpret_cast<uintptr_t>(ConsoleVTable[Index]);
            const uintptr_t Test = FindInstruction(Function, Function + 0x30, { 0xF6, 0x81, -1, -1, 0x00, 0x00, 0x01 });
            const uintptr_t Call = Test ? FindInstruction(Test, Test + 0x40, { 0xB2, 0x01, 0xFF, 0x90 }) : 0;
            if (Call)
            {
                Natives::UConsole_bIsRuntimeAutoCompleteUpToDate = *reinterpret_cast<const int32_t*>(Test + 2);
                Natives::UConsole_BuildRuntimeAutoCompleteListIndex = *reinterpret_cast<const int32_t*>(Call + 4) / static_cast<int32_t>(sizeof(void*));
                Natives::UConsole_InputKeyIndex = Index + 10;
            }
        }

        const ptrdiff_t UpToDate = Natives::UConsole_bIsRuntimeAutoCompleteUpToDate;
        const ptrdiff_t Settings = UpToDate + 0x44;
        const uint8_t SettingsStore[] = { 0x48, 0x89, 0x83, static_cast<uint8_t>(Settings), static_cast<uint8_t>(Settings >> 8), 0x00, 0x00 };
        const uintptr_t ConstructorEnd = Finder::FunctionEnd(Constructor);
        bool bSettingsStored = false;
        for (uintptr_t Scan = Constructor; UpToDate > 0 && Scan + sizeof(SettingsStore) <= ConstructorEnd && !bSettingsStored; ++Scan)
        {
            bSettingsStored = memcmp(reinterpret_cast<const void*>(Scan), SettingsStore, sizeof(SettingsStore)) == 0;
        }
        if (!bSettingsStored)
        {
            Log::Error(L"Finder: UConsole::UpdateCompleteIndices not found or the console layout differs from 4.16");
            ++MissingCount;
            return;
        }

        Natives::UConsole_ConsoleCommandIndex = Finder::VTableIndex(ConsoleVTable, reinterpret_cast<void*>(FunctionOf(L"\n>>> %s <<<")), 0x60);
        if (Natives::UConsole_ConsoleCommandIndex < 0)
        {
            Log::Error(L"Finder: UConsole::ConsoleCommand not found");
            ++MissingCount;
            return;
        }

        Natives::UConsole_AutoCompleteList = UpToDate - 0x1C;
        Natives::UConsole_AutoCompleteTree = UpToDate + 0x0C;
        Natives::UConsole_ConsoleSettings = Settings;
        Log::Info(L"Finder: UConsole::BuildRuntimeAutoCompleteList is slot 0x{:X} in {} console vtables", Natives::UConsole_BuildRuntimeAutoCompleteListIndex, Sites::ConsoleVTables.size());
    }

    uintptr_t AddConsoleObject = 0;

    uintptr_t ScanRipTarget(std::initializer_list<int32_t> Opcode, uint32_t OperandOffset, uint32_t Length, uintptr_t Target, int32_t& Matches)
    {
        uintptr_t Found = 0;
        Matches = 0;
        for (uintptr_t Site = FindInstruction(Finder::TextBegin(), Finder::TextEnd(), Opcode); Site; Site = FindInstruction(Site + 1, Finder::TextEnd(), Opcode))
        {
            if (Finder::RelativeTarget(Site, OperandOffset, Length) == Target)
            {
                Found = Site;
                ++Matches;
            }
        }
        return Found;
    }

    void ResolveConsoleManagerGlobals()
    {
        const uintptr_t SetupSingleton = FunctionOf(L"r.DumpingMovie");
        const bool bSingletonCompare = SetupSingleton && Byte(SetupSingleton + 4) == 0x48 && Byte(SetupSingleton + 5) == 0x83 && Byte(SetupSingleton + 6) == 0x3D && Byte(SetupSingleton + 11) == 0x00;
        Require(L"IConsoleManager::SetupSingleton", Natives::IConsoleManager_SetupSingleton, bSingletonCompare ? SetupSingleton : 0);
        RequireAddress(L"IConsoleManager::Singleton", reinterpret_cast<uintptr_t&>(Natives::IConsoleManager_Singleton), bSingletonCompare ? Finder::RelativeTarget(SetupSingleton + 4, 3, 8) : 0);
        AddConsoleObject = FunctionOf(L"Console object named '%s' already exists but is being registered again, but we weren't expected it to be! (FConsoleManager::AddConsoleObject)");
        RequireAddress(L"FConsoleManager::AddConsoleObject", AddConsoleObject, AddConsoleObject);
    }

    void ResolveAutoCompleteSources()
    {
        uintptr_t Entries = 0;
        int32_t Matches = 0;
        const uintptr_t Handler = FunctionOf(L"showdebug SIGNIFICANCEMANAGER");
        const uintptr_t Bind = Handler ? ScanRipTarget({ 0x4C, 0x8D, 0x05 }, 3, 7, Handler, Matches) : 0;
        if (const uintptr_t Lea = Matches == 1 ? FindInstruction(Bind + 7, Bind + 0x18, { 0x48, 0x8D, 0x0D }) : 0)
        {
            Entries = Finder::RelativeTarget(Lea, 3, 7);
            if (*reinterpret_cast<const int32_t*>(Entries + 0x60) != 2)
            {
                Entries = 0;
            }
        }
        RequireAddress(L"UConsole::RegisterConsoleAutoCompleteEntries", reinterpret_cast<uintptr_t&>(Natives::UConsole_RegisterConsoleAutoCompleteEntries), Entries);


        uintptr_t GameDir = 0;
        const std::vector<uintptr_t> GameDirRefs = Finder::StringRefs(L"../../");
        if (!GameDirRefs.empty())
        {
            GameDir = Finder::FunctionStart(GameDirRefs.front());
            for (const uintptr_t Ref : GameDirRefs)
            {
                if (Finder::FunctionStart(Ref) != GameDir)
                {
                    GameDir = 0;
                }
            }
        }
        Require(L"FPlatformMisc::GameDir", Natives::FPlatformMisc_GameDir, GameDir);

        uintptr_t UserName = 0;
        const uintptr_t GetUserName = Finder::ImportSlot("ADVAPI32.dll", "GetUserNameW");
        for (uintptr_t Call = GetUserName ? FindInstruction(Finder::TextBegin(), Finder::TextEnd(), { 0xFF, 0x15 }) : 0; Call; Call = FindInstruction(Call + 1, Finder::TextEnd(), { 0xFF, 0x15 }))
        {
            if (Finder::RelativeTarget(Call, 2, 6) != GetUserName)
            {
                continue;
            }
            const uintptr_t Caller = Finder::FunctionStart(Call);
            if (UserName && Caller != UserName)
            {
                UserName = 0;
                break;
            }
            UserName = Caller;
        }
        Require(L"FPlatformProcess::UserName", Natives::FPlatformProcess_UserName, UserName);

        uintptr_t MapExtension = 0;
        for (const uintptr_t Ref : Finder::StringRefs(L".umap"))
        {
            if (Byte(Ref - 7) == 0x48 && Byte(Ref - 6) == 0x8B && Byte(Ref - 5) == 0x0D)
            {
                MapExtension = Finder::RelativeTarget(Ref - 7, 3, 7);
            }
        }
        RequireAddress(L"FPackageName::MapPackageExtension", reinterpret_cast<uintptr_t&>(Natives::FPackageName_MapPackageExtension), MapExtension);

        uintptr_t FindPackages = 0;
        for (const uintptr_t Ref : MapExtension ? Finder::StringRefs(L"*.*") : std::vector<uintptr_t>())
        {
            const uintptr_t Function = Finder::FunctionStart(Ref);
            const uintptr_t End = Finder::FunctionEnd(Function);
            for (uintptr_t Lea = Function && End ? FindInstruction(Function, End, { 0x48, 0x8D, 0x15 }) : 0; Lea; Lea = FindInstruction(Lea + 1, End, { 0x48, 0x8D, 0x15 }))
            {
                if (Finder::RelativeTarget(Lea, 3, 7) == MapExtension)
                {
                    FindPackages = Function;
                    break;
                }
            }
        }
        Require(L"FPackageName::FindPackagesInDirectory", Natives::FPackageName_FindPackagesInDirectory, FindPackages);

        Sites::ShowFlags.clear();
        const uintptr_t ShowFlagSink = Finder::FunctionStart(First(Finder::StringRefs("ShowFlag.")));
        for (const uintptr_t Call : ShowFlagSink ? Finder::CallsTo(ShowFlagSink, false) : std::vector<uintptr_t>())
        {
            Sites::FShowFlag ShowFlag;
            if (Byte(Call - 5) == 0x33 && Byte(Call - 4) == 0xD2)
            {
                ShowFlag.Index = 0;
            }
            else if (Byte(Call - 8) == 0xBA)
            {
                ShowFlag.Index = *reinterpret_cast<const uint32_t*>(Call - 7);
            }
            else
            {
                continue;
            }
            for (uintptr_t Lea = FindInstruction(Call - 0x30, Call, { 0x48, 0x8D, 0x15 }); Lea; Lea = FindInstruction(Lea + 1, Call, { 0x48, 0x8D, 0x15 }))
            {
                ShowFlag.Name = reinterpret_cast<const wchar_t*>(Finder::RelativeTarget(Lea, 3, 7));
            }
            if (ShowFlag.Name)
            {
                Sites::ShowFlags.push_back(ShowFlag);
            }
        }
        std::sort(Sites::ShowFlags.begin(), Sites::ShowFlags.end(), [](const Sites::FShowFlag& A, const Sites::FShowFlag& B) { return A.Index < B.Index; });
        for (size_t Index = 0; Index < Sites::ShowFlags.size(); ++Index)
        {
            if (Sites::ShowFlags[Index].Index != Index)
            {
                Sites::ShowFlags.clear();
                break;
            }
        }
        if (Sites::ShowFlags.empty())
        {
            Log::Error(L"Finder: the engine show flags were not found");
            ++MissingCount;
        }
    }

    void ResolveViewportClient()
    {
        const uintptr_t GEngine = reinterpret_cast<uintptr_t>(Natives::GEngine);
        uintptr_t Setup = 0;
        int32_t Matches = 0;
        for (uintptr_t Site = FindInstruction(Finder::TextBegin(), Finder::TextEnd(), { 0xC6, 0x41, -1, 0x00, 0x48, 0x8B, 0x0D }); Site; Site = FindInstruction(Site + 1, Finder::TextEnd(), { 0xC6, 0x41, -1, 0x00, 0x48, 0x8B, 0x0D }))
        {
            if (GEngine && Finder::RelativeTarget(Site + 4, 3, 7) == GEngine && Byte(Site + 11) == 0x48 && Byte(Site + 12) == 0x8D && Byte(Site + 13) == 0x05)
            {
                Setup = Finder::FunctionStart(Site);
                ++Matches;
            }
        }
        if (Matches != 1)
        {
            Log::Error(L"Finder: expected 1 UGameViewportClient::SetupInitialLocalPlayer, found {}", Matches);
            Setup = 0;
        }
        Require(L"UGameViewportClient::SetupInitialLocalPlayer", Natives::UGameViewportClient_SetupInitialLocalPlayer, Setup);
    }

    void RequireSlot(const wchar_t* Name, int32_t& Destination, int32_t Index)
    {
        if (Index < 0)
        {
            Log::Error(L"Finder: {} not found", Name);
            ++MissingCount;
            return;
        }
        Destination = Index;
    }

    void RequireOffset(const wchar_t* Name, ptrdiff_t& Destination, ptrdiff_t Offset)
    {
        if (Offset <= 0)
        {
            Log::Error(L"Finder: {} not found", Name);
            ++MissingCount;
            return;
        }
        Destination = Offset;
    }

    uintptr_t FindVirtualCall(uintptr_t Begin, uintptr_t End)
    {
        for (uintptr_t Site = Begin; Site + 6 <= End; ++Site)
        {
            const uint8_t ModRm = Byte(Site + 1);
            if (Byte(Site) == 0xFF && (ModRm & 0xC0) == 0x80 && (ModRm & 0x07) != 0x04 && ((ModRm & 0x38) == 0x10 || (ModRm & 0x38) == 0x20))
            {
                return Site;
            }
        }
        return 0;
    }

    int32_t VirtualSlot(uintptr_t Site)
    {
        const int32_t Displacement = Site ? *reinterpret_cast<const int32_t*>(Site + 2) : 0;
        return Displacement > 0 && Displacement % 8 == 0 ? Displacement / 8 : -1;
    }

    uintptr_t VTableEntry(void** VTable, int32_t Index)
    {
        if (!VTable || Index < 0 || Index >= 0x400)
        {
            return 0;
        }
        const uintptr_t Entry = reinterpret_cast<uintptr_t>(VTable[Index]);
        return IsInText(Entry) ? Entry : 0;
    }

    // Without RTTI the vtables sit back to back, so a scan runs on into the subclasses' vtables and meets the same
    // function again: the class's own slot is the first match. Slots Core has already swapped are stepped over.
    template <typename TPredicate>
    int32_t FirstVTableIndexWhere(void** VTable, TPredicate&& Predicate)
    {
        for (int32_t Index = 0; VTable && Index < 0x400; ++Index)
        {
            const uintptr_t Entry = reinterpret_cast<uintptr_t>(VTable[Index]);
            if (IsInText(Entry))
            {
                if (Predicate(Entry))
                {
                    return Index;
                }
            }
            else if (!IsCoreFunction(Entry))
            {
                break;
            }
        }
        return -1;
    }

    int32_t FirstVTableIndex(void** VTable, uintptr_t Function)
    {
        return Function ? FirstVTableIndexWhere(VTable, [Function](uintptr_t Entry) { return Entry == Function; }) : -1;
    }

    struct FCalledSlot
    {
        int32_t Index = -1;
        void** VTable = nullptr;
        uintptr_t Reference = 0;
    };

    FCalledSlot CalledSlot(uintptr_t Begin, uintptr_t End, uintptr_t Function)
    {
        const uintptr_t Entry = reinterpret_cast<uintptr_t>(Finder::VTableContaining(Function, 0, 1));
        std::vector<std::pair<int32_t, uintptr_t>> Candidates;
        for (uintptr_t Site = Entry && Begin ? FindVirtualCall(Begin, End) : 0; Site; Site = FindVirtualCall(Site + 1, End))
        {
            const int32_t Index = VirtualSlot(Site);
            if (Index >= 0)
            {
                Candidates.emplace_back(Index, Entry - Index * sizeof(void*));
            }
        }

        FCalledSlot Result;
        if (Candidates.empty())
        {
            return Result;
        }

        const uintptr_t TextEnd = Finder::TextEnd();
        for (uintptr_t Lea = Finder::TextBegin(); Lea + 7 <= TextEnd; ++Lea)
        {
            if ((Byte(Lea) & 0xFB) != 0x48 || Byte(Lea + 1) != 0x8D || (Byte(Lea + 2) & 0xC7) != 0x05)
            {
                continue;
            }
            const uintptr_t Target = Finder::RelativeTarget(Lea, 3, 7);
            for (const auto& [Index, VTable] : Candidates)
            {
                if (VTable != Target)
                {
                    continue;
                }
                if (Result.VTable && Result.VTable != reinterpret_cast<void**>(VTable))
                {
                    return {};
                }
                Result = { Index, reinterpret_cast<void**>(VTable), Lea };
            }
        }
        return Result;
    }

    std::vector<std::pair<uintptr_t, uintptr_t>> NativeThunks(const char* Name)
    {
        std::vector<std::pair<uintptr_t, uintptr_t>> Thunks;
        for (const uintptr_t Ref : Finder::StringRefs(Name))
        {
            if (Byte(Ref - 7) == 0x4C && Byte(Ref - 6) == 0x8D && Byte(Ref - 5) == 0x05)
            {
                Thunks.emplace_back(Finder::FunctionStart(Ref), Finder::RelativeTarget(Ref - 7, 3, 7));
            }
        }
        return Thunks;
    }

    void ResolveReplication()
    {
        const uintptr_t ReplicateActor = FunctionOf(L"ReplicateActor: bPausedUntilReliableACK is ending now that reliables have been ACK'd. %s");
        const uintptr_t ReplicateActorEnd = Finder::FunctionEnd(ReplicateActor);
        const uintptr_t SetChannelActor = FunctionOf(L"SetChannelActor: ChIndex: %i, Actor: %s, NetGUID: %s");
        const uintptr_t NetViewer = Finder::FunctionStart(First(Finder::StringRefs("ServerForwardView")));
        const uintptr_t NetViewerEnd = Finder::FunctionEnd(NetViewer);
        Require(L"UActorChannel::ReplicateActor", Natives::UActorChannel_ReplicateActor, ReplicateActor);
        Require(L"UActorChannel::SetChannelActor", Natives::UActorChannel_SetChannelActor, SetChannelActor);
        Require(L"FNetViewer::FNetViewer", Natives::FNetViewer_FNetViewer, NetViewer);
        Require(L"FNetGUIDCache::SupportsObject", Natives::FNetGUIDCache_SupportsObject, FunctionOf(L"FNetGUIDCache::SupportsObject: %s NOT Supported."));
        Require(L"FObjectReplicator::ValidateAgainstState", Natives::FObjectReplicator_ValidateAgainstState, FunctionOf(L"ValidateAgainstState: Properties changed for %s"));

        uintptr_t CreateChannel = 0;
        const uintptr_t BunchCreate = OnlyRef(L"      Bunch Create %i: ChType %i, bReliable: %i, bPartial: %i, bPartialInitial: %i, bPartialFinal: %i");
        for (uintptr_t Site = BunchCreate ? FindRawCall(BunchCreate, BunchCreate + 0x40) : 0; Site && !CreateChannel; Site = FindRawCall(Site + 5, BunchCreate + 0x40))
        {
            const uintptr_t Target = Finder::RelativeTarget(Site, 1, 5);
            if (FindInstruction(Site - 0x10, Site, { 0x45, 0x33, 0xC0 }) && FindInstruction(Target, Target + 0x80, { 0x00, 0x28, 0x00, 0x00 }))
            {
                CreateChannel = Target;
            }
        }
        Require(L"UNetConnection::CreateChannel", Natives::UNetConnection_CreateChannel, CreateChannel);

        const uintptr_t DemoReplicate = FunctionOf(L"DemoReplicateActor: bMustReplicate is true but nothing was sent: %s");
        uintptr_t PreReplication = 0;
        size_t PreReplicationSites = 0;
        for (const uintptr_t Site : DemoReplicate ? Finder::CallsTo(DemoReplicate, false) : std::vector<uintptr_t>())
        {
            uintptr_t Nearest = 0;
            for (uintptr_t Call = FindRawCall(Site - 0x20, Site); Call; Call = FindRawCall(Call + 1, Site))
            {
                Nearest = Call;
            }
            const uintptr_t Target = Nearest ? Finder::RelativeTarget(Nearest, 1, 5) : 0;
            if (!Target || (PreReplication && PreReplication != Target))
            {
                PreReplicationSites = 0;
                break;
            }
            PreReplication = Target;
            ++PreReplicationSites;
        }
        Require(L"AActor::CallPreReplication", Natives::AActor_CallPreReplication, PreReplicationSites >= 2 ? PreReplication : 0);

        const uintptr_t NotifyDestroyed = FunctionOf(L"NotifyActorDestroyed %s - StartupActor");
        const uintptr_t NotifyDestroyedEnd = Finder::FunctionEnd(NotifyDestroyed);
        const std::vector<std::pair<uintptr_t, uintptr_t>> NotifyCalls = NotifyDestroyed && NotifyDestroyedEnd ? Finder::DirectCalls(NotifyDestroyed, NotifyDestroyedEnd) : std::vector<std::pair<uintptr_t, uintptr_t>>();

        uintptr_t ObjectListRemove = 0;
        ptrdiff_t NetworkObjects = 0;
        if (!NotifyCalls.empty())
        {
            const uintptr_t Load = NotifyCalls.back().first - 7;
            if ((Byte(Load) & 0xFE) == 0x48 && Byte(Load + 1) == 0x8B && (Byte(Load + 2) & 0xF8) == 0x88 && (Byte(Load + 2) & 0x07) != 0x04)
            {
                ObjectListRemove = NotifyCalls.back().second;
                NetworkObjects = *reinterpret_cast<const int32_t*>(Load + 3);
            }
        }
        Require(L"FNetworkObjectList::Remove", Natives::FNetworkObjectList_Remove, ObjectListRemove);
        RequireOffset(L"UNetDriver::NetworkObjects", Natives::UNetDriver_NetworkObjects, NetworkObjects);

        ptrdiff_t ActorChannels = 0;
        int32_t ActorChannelLookups = 0;
        for (uintptr_t Load = NotifyDestroyed; NotifyDestroyedEnd && Load + 12 <= NotifyDestroyedEnd; ++Load)
        {
            if (Byte(Load) == 0x8B && (Byte(Load + 1) & 0xC0) == 0x80 && (Byte(Load + 1) & 0x07) != 0x04 && Byte(Load + 6) == 0x2B && (Byte(Load + 7) & 0xC0) == 0x80 && (Byte(Load + 7) & 0x07) != 0x04
                && *reinterpret_cast<const int32_t*>(Load + 8) - *reinterpret_cast<const int32_t*>(Load + 2) == 0x2C)
            {
                ActorChannels = *reinterpret_cast<const int32_t*>(Load + 2) - 8;
                ++ActorChannelLookups;
            }
        }
        RequireOffset(L"UNetConnection::ActorChannels", Natives::UNetConnection_ActorChannels, ActorChannelLookups == 1 ? ActorChannels : 0);

        uintptr_t CreateDestructionInfo = 0;
        ptrdiff_t ConnectionDestroyedActors = 0;
        int32_t DestroyedActorAdds = 0;
        for (const auto& [Site, Target] : NotifyCalls)
        {
            const uintptr_t Lea = Site + 5;
            if (Byte(Lea) == 0x48 && Byte(Lea + 1) == 0x8D && (Byte(Lea + 2) & 0xF8) == 0x88 && (Byte(Lea + 2) & 0x07) != 0x04 && FindInstruction(Lea + 7, Lea + 0x18, { 0x4C, 0x8D, 0x40 }))
            {
                CreateDestructionInfo = Target;
                ConnectionDestroyedActors = *reinterpret_cast<const int32_t*>(Lea + 3);
                ++DestroyedActorAdds;
            }
        }
        RequireOffset(L"UNetConnection::DestroyedStartupOrDormantActors", Natives::UNetConnection_DestroyedStartupOrDormantActors, DestroyedActorAdds == 1 ? ConnectionDestroyedActors : 0);

        ptrdiff_t DormantReplicators = 0;
        int32_t DormantReplicatorRemoves = 0;
        for (uintptr_t Lea = NotifyDestroyed; ActorChannels > 0 && DestroyedActorAdds == 1 && Lea + 7 <= NotifyDestroyedEnd; ++Lea)
        {
            const ptrdiff_t Offset = *reinterpret_cast<const int32_t*>(Lea + 3);
            if (Byte(Lea) == 0x48 && Byte(Lea + 1) == 0x8D && (Byte(Lea + 2) & 0xF8) == 0x88 && (Byte(Lea + 2) & 0x07) != 0x04 && Offset > ActorChannels && Offset < ConnectionDestroyedActors)
            {
                DormantReplicators = Offset;
                ++DormantReplicatorRemoves;
            }
        }
        RequireOffset(L"UNetConnection::DormantReplicatorMap", Natives::UNetConnection_DormantReplicatorMap, DormantReplicatorRemoves == 1 ? DormantReplicators : 0);

        const uintptr_t ActorChannelClose = FunctionOf(L"UActorChannel::Close: ChIndex: %d, Actor: %s");
        ptrdiff_t ActorChannelBits = 0;
        FCalledSlot ChannelClose;
        for (uintptr_t And = NotifyDestroyedEnd ? FindInstruction(NotifyDestroyed, NotifyDestroyedEnd, { 0x83, -1, -1, -1, 0x00, 0x00, 0xF7 }) : 0; And && !ChannelClose.VTable; And = FindInstruction(And + 1, NotifyDestroyedEnd, { 0x83, -1, -1, -1, 0x00, 0x00, 0xF7 }))
        {
            if ((Byte(And + 1) & 0xF8) == 0xA0 && (Byte(And + 1) & 0x07) != 0x04)
            {
                ChannelClose = CalledSlot(And + 7, And + 0x10, ActorChannelClose);
                ActorChannelBits = *reinterpret_cast<const int32_t*>(And + 2);
            }
        }
        void** ActorChannelVTable = ChannelClose.VTable;
        RequireOffset(L"UActorChannel::Bits", Natives::UActorChannel_Bits, ActorChannelVTable ? ActorChannelBits : 0);
        RequireSlot(L"UChannel::Close", Natives::UChannel_CloseIndex, ChannelClose.Index);
        RequireSlot(L"UChannel::StartBecomingDormant", Natives::UChannel_StartBecomingDormantIndex, Finder::VTableIndex(ActorChannelVTable, reinterpret_cast<void*>(FunctionOf(L"StartBecomingDormant: %s")), 0x400));

        const uintptr_t Close = FunctionOf(L"UChannel::Close: Sending CloseBunch. Dormant: %d, %s");
        const uintptr_t CloseEnd = Finder::FunctionEnd(Close);
        const uintptr_t ClosingTest = Close ? FindInstruction(Close, Close + 0x20, { 0xF6, 0x41, -1, 0x02 }) : 0;
        RequireOffset(L"UChannel::Bits", Natives::UChannel_Bits, ClosingTest ? Byte(ClosingTest + 2) : 0);

        ptrdiff_t State = 0;
        for (uintptr_t Load = CloseEnd ? FindInstruction(Close, CloseEnd, { 0x8B, -1, -1, -1, 0x00, 0x00, 0x83, -1, 0x02, 0x83, -1, 0x01 }) : 0; Load && !State; Load = FindInstruction(Load + 1, CloseEnd, { 0x8B, -1, -1, -1, 0x00, 0x00, 0x83, -1, 0x02, 0x83, -1, 0x01 }))
        {
            if ((Byte(Load + 1) & 0xC0) == 0x80 && (Byte(Load + 1) & 0x07) != 0x04)
            {
                State = *reinterpret_cast<const int32_t*>(Load + 2);
            }
        }
        RequireOffset(L"UNetConnection::State", Natives::UNetConnection_State, State);

        const uintptr_t CloseBunch = CloseEnd ? FindInstruction(Close, CloseEnd, { 0x41, 0xB0, 0x01, 0x48, 0x8D, 0x4C, 0x24, -1, 0x48, 0x8B, 0xD3, 0xE8 }) : 0;
        const uintptr_t OutBunch = CloseBunch ? Finder::RelativeTarget(CloseBunch + 11, 1, 5) : 0;
        Require(L"FOutBunch::FOutBunch", Natives::FOutBunch_FOutBunch, IsInText(OutBunch) ? OutBunch : 0);
        const int32_t SendBunch = CloseBunch ? VirtualSlot(FindVirtualCall(CloseBunch + 16, CloseBunch + 0x60)) : -1;
        RequireSlot(L"UChannel::SendBunch", Natives::UChannel_SendBunchIndex, VTableEntry(ActorChannelVTable, SendBunch) ? SendBunch : -1);

        ptrdiff_t ChIndex = 0;
        for (uintptr_t Copy = IsInText(OutBunch) ? FindInstruction(OutBunch, OutBunch + 0x80, { 0x8B, -1, -1, 0x89, -1, -1, -1, 0x00, 0x00 }) : 0; Copy && !ChIndex; Copy = FindInstruction(Copy + 1, OutBunch + 0x80, { 0x8B, -1, -1, 0x89, -1, -1, -1, 0x00, 0x00 }))
        {
            const bool bLoadsChannel = (Byte(Copy + 1) & 0xC0) == 0x40 && (Byte(Copy + 1) & 0x07) != 0x04;
            const bool bStoresBunch = (Byte(Copy + 4) & 0xC0) == 0x80 && (Byte(Copy + 4) & 0x07) != 0x04 && (Byte(Copy + 4) & 0x38) == (Byte(Copy + 1) & 0x38);
            if (bLoadsChannel && bStoresBunch)
            {
                ChIndex = Byte(Copy + 2);
            }
        }
        RequireOffset(L"UChannel::ChIndex", Natives::UChannel_ChIndex, ChIndex);

        // The bunch flags are bool bytes on 1.1 and 1.2 (bClose +0xE5, bDormant +0xE6, bReliable +0xE8) and uint8 :1 bits
        // of +0xE0 on 1.7.2 (bClose 0x04, bDormant 0x08, bReliable 0x20): the constructor (0x20A4210) shifts bInClose
        // into place before the store, and UChannel::Close (0x20A8690) sets bDormant and bReliable in one
        // read-modify-write of the flag byte.
        ptrdiff_t BunchClose = 0;
        uint32_t CloseMask = 0;
        const uintptr_t OutBunchEnd = IsInText(OutBunch) ? Finder::FunctionEnd(OutBunch) : 0;
        const uintptr_t CloseArgument = OutBunchEnd ? FindInstruction(OutBunch, OutBunchEnd, { 0x41, 0x0F, 0xB6, -1 }) : 0;
        if (CloseArgument && (Byte(CloseArgument + 3) & 0xC7) == 0xC0)
        {
            const uint8_t Register = (Byte(CloseArgument + 3) >> 3) & 0x07;
            uint32_t Shift = 0;
            for (uintptr_t Store = CloseArgument + 4; Store + 6 <= OutBunchEnd && !BunchClose; ++Store)
            {
                const uint8_t ModRm = Byte(Store + 1);
                const bool bNeedsRex = Register >= 4;
                if (Byte(Store) == 0xC0 && ModRm == (0xE0 | Register) && Byte(Store + 2) < 8)
                {
                    Shift = Byte(Store + 2);
                }
                else if (Byte(Store) == 0x88 && (ModRm & 0xC0) == 0x80 && ((ModRm >> 3) & 0x07) == Register && (ModRm & 0x07) != 0x04 && (!bNeedsRex || Byte(Store - 1) == 0x40))
                {
                    BunchClose = *reinterpret_cast<const int32_t*>(Store + 2);
                    CloseMask = 1u << Shift;
                }
            }
        }

        ptrdiff_t BunchDormant = 0;
        ptrdiff_t BunchReliable = 0;
        uint32_t DormantMask = 0;
        uint32_t ReliableMask = 0;
        const ptrdiff_t CloseBunchLocal = CloseBunch ? Byte(CloseBunch + 7) : 0;
        if (CloseBunch && CloseMask == 1)
        {
            if (const uintptr_t Store = FindInstruction(CloseBunch + 16, CloseBunch + 0x60, { 0xC6, 0x84, 0x24, -1, -1, -1, -1, 0x01 }))
            {
                BunchReliable = *reinterpret_cast<const int32_t*>(Store + 3) - CloseBunchLocal;
            }
            if (const uintptr_t Store = FindInstruction(CloseBunch + 16, CloseBunch + 0x60, { 0x88, 0x84, 0x24 }))
            {
                BunchDormant = *reinterpret_cast<const int32_t*>(Store + 3) - CloseBunchLocal;
            }
            DormantMask = ReliableMask = 1;
        }
        else if (CloseBunch && CloseMask)
        {
            // movzx eax, byte [rsp+Flags]; and al, ~Dormant; or cl, R; shl cl, N; mov [rsp+Flags], cl
            const int32_t Flags = static_cast<int32_t>(CloseBunchLocal + BunchClose);
            const int32_t F0 = Flags & 0xFF, F1 = (Flags >> 8) & 0xFF, F2 = (Flags >> 16) & 0xFF, F3 = (Flags >> 24) & 0xFF;
            const uintptr_t Load = FindInstruction(CloseBunch + 16, CloseBunch + 0x60, { 0x0F, 0xB6, 0x84, 0x24, F0, F1, F2, F3 });
            const uintptr_t Store = Load ? FindInstruction(Load + 8, CloseBunch + 0x60, { 0x88, -1, 0x24, F0, F1, F2, F3 }) : 0;
            const uintptr_t Clear = Store ? FindInstruction(Load + 8, Store, { 0x24, static_cast<int32_t>(~(CloseMask << 1) & 0xFF) }) : 0;
            const uintptr_t Shl = Store ? FindInstruction(Load + 8, Store, { 0xC0, 0xE1, -1 }) : 0;
            const uintptr_t Or = Shl ? FindInstruction(Load + 8, Shl, { 0x80, 0xC9, -1 }) : 0;
            if (Clear && Or && (Byte(Store + 1) & 0xC7) == 0x84)
            {
                BunchDormant = BunchReliable = BunchClose;
                DormantMask = 1u << Byte(Shl + 2);
                ReliableMask = static_cast<uint32_t>(Byte(Or + 2)) << Byte(Shl + 2);
            }
        }
        // UE 4.16 declares bOpen, bClose, bDormant, bIsReplicationPaused, bReliable in this order, as bytes or as bits.
        const bool bBunchFlags = BunchClose > 0 && (CloseMask == 1
            ? BunchDormant == BunchClose + 1 && BunchReliable == BunchClose + 3
            : CloseMask && BunchDormant == BunchClose && BunchReliable == BunchClose && DormantMask == CloseMask << 1 && ReliableMask == CloseMask << 3);
        RequireOffset(L"FOutBunch::bClose", Natives::FOutBunch_bClose, bBunchFlags ? BunchClose : 0);
        RequireOffset(L"FOutBunch::bDormant", Natives::FOutBunch_bDormant, bBunchFlags ? BunchDormant : 0);
        RequireOffset(L"FOutBunch::bReliable", Natives::FOutBunch_bReliable, bBunchFlags ? BunchReliable : 0);
        Natives::FOutBunch_bCloseMask = bBunchFlags ? CloseMask : 0;
        Natives::FOutBunch_bDormantMask = bBunchFlags ? DormantMask : 0;
        Natives::FOutBunch_bReliableMask = bBunchFlags ? ReliableMask : 0;

        ptrdiff_t BunchSize = 0;
        for (uintptr_t Lea = OutBunchEnd ? FindInstruction(OutBunch, OutBunchEnd, { 0x48, 0x8D, 0x05 }) : 0; Lea && !BunchSize; Lea = FindInstruction(Lea + 1, OutBunchEnd, { 0x48, 0x8D, 0x05 }))
        {
            void** VTable = reinterpret_cast<void**>(Finder::RelativeTarget(Lea, 3, 7));
            const uintptr_t Destructor = !IsInText(reinterpret_cast<uintptr_t>(VTable)) ? reinterpret_cast<uintptr_t>(VTable[0]) : 0;
            const uintptr_t DestructorEnd = IsInText(Destructor) ? Finder::FunctionEnd(Destructor) : 0;
            for (uintptr_t Size = DestructorEnd ? FindInstruction(Destructor, DestructorEnd, { 0xBA, -1, -1, 0x00, 0x00 }) : 0; Size && !BunchSize; Size = FindInstruction(Size + 1, DestructorEnd, { 0xBA, -1, -1, 0x00, 0x00 }))
            {
                if (FindRawCall(Size + 5, Size + 0x10))
                {
                    BunchSize = *reinterpret_cast<const int32_t*>(Size + 1);
                }
            }
        }
        RequireOffset(L"sizeof(FOutBunch)", Natives::FOutBunch_Size, BunchSize > BunchReliable && BunchSize <= static_cast<ptrdiff_t>(FOutBunch::StorageSize) ? BunchSize : 0);

        const uintptr_t Overflowed = FunctionOf(L"FBitWriter overflowed! (WriteLen: %i, Remaining: %i, Max: %i)");
        const uintptr_t OverflowedEnd = Overflowed ? Finder::FunctionEnd(Overflowed) : 0;
        ptrdiff_t ErrorByte = 0;
        uint32_t ErrorMask = 0;
        for (uintptr_t Or = OverflowedEnd ? FindInstruction(Overflowed, OverflowedEnd, { 0x80, -1, -1, -1 }) : 0; Or && !ErrorMask; Or = FindInstruction(Or + 1, OverflowedEnd, { 0x80, -1, -1, -1 }))
        {
            if ((Byte(Or + 1) & 0xF8) == 0x48 && (Byte(Or + 1) & 0x07) != 0x04 && Byte(Or + 3) && (Byte(Or + 3) & (Byte(Or + 3) - 1)) == 0)
            {
                ErrorByte = Byte(Or + 2);
                ErrorMask = Byte(Or + 3);
            }
        }
        RequireOffset(L"FArchive::ArIsError", Natives::FArchive_ArIsError, ErrorByte);
        Natives::FArchive_ArIsErrorMask = ErrorMask;

        ptrdiff_t WriterNum = 0;
        const std::initializer_list<int32_t> SubtractNum = { -1, 0x2B, -1, -1, -1, -1, -1 };
        const std::initializer_list<int32_t> LoadMax = { 0x48, 0x8B, -1, -1, -1, -1, -1 };
        for (uintptr_t Sub = OverflowedEnd ? FindInstruction(Overflowed, OverflowedEnd, SubtractNum) : 0; Sub && !WriterNum; Sub = FindInstruction(Sub + 1, OverflowedEnd, SubtractNum))
        {
            const uint8_t ModRm = Byte(Sub + 2);
            if ((Byte(Sub) & 0xF8) != 0x48 || (ModRm & 0xC0) != 0x80 || (ModRm & 0x07) == 0x04)
            {
                continue;
            }
            const ptrdiff_t Num = *reinterpret_cast<const int32_t*>(Sub + 3);
            const uintptr_t From = Sub > Overflowed + 0x20 ? Sub - 0x20 : Overflowed;
            for (uintptr_t Load = FindInstruction(From, Sub, LoadMax); Load && !WriterNum; Load = FindInstruction(Load + 1, Sub, LoadMax))
            {
                if ((Byte(Load + 2) & 0xC7) == (0x80 | (ModRm & 0x07)) && *reinterpret_cast<const int32_t*>(Load + 3) == Num + 8)
                {
                    WriterNum = Num;
                }
            }
        }
        RequireOffset(L"FBitWriter::Num", Natives::FBitWriter_Num, WriterNum);
        Log::Info(L"Finder: FOutBunch is 0x{:X} bytes, bClose +0x{:X} & 0x{:X}, bDormant +0x{:X} & 0x{:X}, bReliable +0x{:X} & 0x{:X}, Num +0x{:X}, ArIsError +0x{:X} & 0x{:X}, UChannel::ChIndex +0x{:X}", BunchSize, BunchClose, CloseMask, BunchDormant, DormantMask, BunchReliable, ReliableMask, WriterNum, ErrorByte, ErrorMask, ChIndex);

        ptrdiff_t NumOutRec = 0;
        const uintptr_t PausedTest = ReplicateActor ? FindInstruction(ReplicateActor, ReplicateActor + 0x80, { 0x0F, 0xBA, -1, 0x08 }) : 0;
        for (uintptr_t Compare = PausedTest && (Byte(PausedTest + 2) & 0x38) == 0x20 ? FindInstruction(PausedTest + 4, PausedTest + 0x14, { 0x83, -1, -1, 0x00 }) : 0; Compare && !NumOutRec; Compare = FindInstruction(Compare + 1, PausedTest + 0x14, { 0x83, -1, -1, 0x00 }))
        {
            if ((Byte(Compare + 1) & 0xF8) == 0x78 && (Byte(Compare + 1) & 0x07) != 0x04)
            {
                NumOutRec = Byte(Compare + 2);
            }
        }
        RequireOffset(L"UChannel::NumOutRec", Natives::UChannel_NumOutRec, NumOutRec);

        ptrdiff_t LastUpdateTime = 0;
        int32_t LastUpdateStores = 0;
        for (uintptr_t Store = ReplicateActorEnd ? FindInstruction(ReplicateActor, ReplicateActorEnd, { 0x0F, 0x5A, 0xC0, 0xF2, -1, 0x0F, 0x11, -1 }) : 0; Store; Store = FindInstruction(Store + 1, ReplicateActorEnd, { 0x0F, 0x5A, 0xC0, 0xF2, -1, 0x0F, 0x11, -1 }))
        {
            if ((Byte(Store + 4) & 0xF0) == 0x40 && (Byte(Store + 7) & 0xC0) == 0x80 && (Byte(Store + 7) & 0x07) != 0x04)
            {
                LastUpdateTime = *reinterpret_cast<const int32_t*>(Store + 8);
                ++LastUpdateStores;
            }
        }
        RequireOffset(L"UActorChannel::LastUpdateTime", Natives::UActorChannel_LastUpdateTime, LastUpdateStores == 1 ? LastUpdateTime : 0);
        RequireOffset(L"UActorChannel::RelevantTime", Natives::UActorChannel_RelevantTime, LastUpdateStores == 1 ? LastUpdateTime - static_cast<ptrdiff_t>(sizeof(double)) : 0);

        uintptr_t WorldSettings = 0;
        int32_t WorldSettingsCalls = 0;
        for (uintptr_t Call = ReplicateActorEnd ? FindInstruction(ReplicateActor, ReplicateActorEnd, { 0x41, 0xB0, 0x01, 0x33, 0xD2, 0x48, 0x8B, 0xC8, 0xE8 }) : 0; Call; Call = FindInstruction(Call + 1, ReplicateActorEnd, { 0x41, 0xB0, 0x01, 0x33, 0xD2, 0x48, 0x8B, 0xC8, 0xE8 }))
        {
            WorldSettings = Finder::RelativeTarget(Call + 8, 1, 5);
            ++WorldSettingsCalls;
        }
        Require(L"UWorld::GetWorldSettings", Natives::UWorld_GetWorldSettings, WorldSettingsCalls == 1 && IsInText(WorldSettings) ? WorldSettings : 0);

        const uintptr_t SerializeNewActor = FunctionOf(L"SerializeNewActor START");
        const uintptr_t SerializeNewActorEnd = Finder::FunctionEnd(SerializeNewActor);
        uintptr_t Archetype = 0;
        int32_t ArchetypeCalls = 0;
        for (uintptr_t Call = SerializeNewActorEnd ? FindInstruction(SerializeNewActor, SerializeNewActorEnd, { 0x49, 0x8B, 0x50, 0x20, 0x49, 0x8B, 0x48, 0x10, 0x45, 0x8B, 0x48, 0x08, 0x4D, 0x8B, 0x40, 0x18, 0xE8 }) : 0; Call; Call = FindInstruction(Call + 1, SerializeNewActorEnd, { 0x49, 0x8B, 0x50, 0x20, 0x49, 0x8B, 0x48, 0x10, 0x45, 0x8B, 0x48, 0x08, 0x4D, 0x8B, 0x40, 0x18, 0xE8 }))
        {
            Archetype = Finder::RelativeTarget(Call + 16, 1, 5);
            ++ArchetypeCalls;
        }
        Require(L"UObject::GetArchetypeFromRequiredInfo", Natives::UObject_GetArchetypeFromRequiredInfo, ArchetypeCalls == 1 && IsInText(Archetype) ? Archetype : 0);

        const FCalledSlot NewActor = ReplicateActorEnd ? CalledSlot(ReplicateActor, ReplicateActorEnd, SerializeNewActor) : FCalledSlot();
        const int32_t WriteObject = Finder::VTableIndex(NewActor.VTable, reinterpret_cast<void*>(FunctionOf(L"WroteObject %s NetGUID <%s>")), 0x400);
        RequireSlot(L"UPackageMap::WriteObject", Natives::UPackageMap_WriteObjectIndex, WriteObject >= 0 && WriteObject == NewActor.Index - 4 ? WriteObject : -1);

        const uintptr_t PlayReplay = FunctionOf(L"PlayReplay: failed to create demo net driver!");
        const uintptr_t PlayReplayEnd = Finder::FunctionEnd(PlayReplay);
        const FCalledSlot InitConnect = PlayReplayEnd ? CalledSlot(PlayReplay, PlayReplayEnd, FunctionOf(L"UNetDriver::InitConnect")) : FCalledSlot();
        void** NetDriverVTable = InitConnect.VTable;

        const int32_t ProcessRemoteFunction = Finder::VTableIndex(NetDriverVTable, reinterpret_cast<void*>(FunctionOf(L"UNetDriver::ProcessRemoteFunction")), 0x400);
        const int32_t LowLevelGetNetworkNumber = Finder::VTableIndex(NetDriverVTable, reinterpret_cast<void*>(FunctionOf(L"UNetDriver::LowLevelGetNetworkNumber")), 0x400);
        const int32_t ServerReplicateActors = ProcessRemoteFunction - 1;
        const uintptr_t StrippedReplicate = ProcessRemoteFunction > 0 && ServerReplicateActors == LowLevelGetNetworkNumber + 2 ? VTableEntry(NetDriverVTable, ServerReplicateActors) : 0;
        const bool bServerReplicateActors = StrippedReplicate && Byte(StrippedReplicate) == 0x33 && Byte(StrippedReplicate + 1) == 0xC0 && Byte(StrippedReplicate + 2) == 0xC3;
        RequireSlot(L"UNetDriver::ServerReplicateActors", Natives::UNetDriver_ServerReplicateActorsIndex, bServerReplicateActors ? ServerReplicateActors : -1);

        Sites::NetDriverVTables.clear();
        if (bServerReplicateActors && InitConnect.Reference)
        {
            std::vector<void**> VTables = { NetDriverVTable };
            CollectDerivedVTables(Finder::FunctionStart(InitConnect.Reference), VTables, 0);
            for (void** VTable : VTables)
            {
                if (VTable[ServerReplicateActors] == NetDriverVTable[ServerReplicateActors])
                {
                    Sites::NetDriverVTables.push_back(VTable);
                }
            }
        }
        if (Sites::NetDriverVTables.empty())
        {
            Log::Error(L"Finder: net driver vtables with the stripped ServerReplicateActors not found");
            ++MissingCount;
        }

        const int32_t TickDispatch = Finder::VTableIndex(NetDriverVTable, reinterpret_cast<void*>(FunctionOf(L"UNetDriver::TickDispatch: Very long time between ticks. DeltaTime: %2.2f, Realtime: %2.2f. %s")), 0x400);
        const uintptr_t TickFlush = TickDispatch >= 0 ? VTableEntry(NetDriverVTable, TickDispatch + 1) : 0;
        const uintptr_t TickFlushEnd = Finder::FunctionEnd(TickFlush);
        const uintptr_t DemoTickFlush = FunctionOf(L"UDemoNetDriver::TickFlush: ReplayStreamer ERROR: %s");
        bool bDemoTickFlush = false;
        for (const uintptr_t Site : TickFlush && DemoTickFlush ? Finder::CallsTo(TickFlush, false) : std::vector<uintptr_t>())
        {
            bDemoTickFlush = bDemoTickFlush || Finder::FunctionStart(Site) == DemoTickFlush;
        }
        RequireSlot(L"UNetDriver::TickFlush", Natives::UNetDriver_TickFlushIndex, bDemoTickFlush ? TickDispatch + 1 : -1);
        Require(L"UNetDriver::TickFlush", Natives::UNetDriver_TickFlush, bDemoTickFlush ? TickFlush : 0);

        const int32_t IsServer = TickFlush ? VirtualSlot(FindVirtualCall(TickFlush, TickFlush + 0x60)) : -1;
        const uintptr_t IsServerBody = VTableEntry(NetDriverVTable, IsServer);
        const uintptr_t GetNetMode = reinterpret_cast<uintptr_t>(Natives::UNetDriver_GetNetMode);
        const bool bIsServer = IsServerBody && FindInstruction(IsServerBody, IsServerBody + 9, { 0x48, 0x83, 0x79, -1, 0x00, 0x0F, 0x94, 0xC0, 0xC3 }) == IsServerBody && GetNetMode && VirtualSlot(FindVirtualCall(GetNetMode, GetNetMode + 0x10)) == IsServer;
        RequireSlot(L"UNetDriver::IsServer", Natives::UNetDriver_IsServerIndex, bIsServer ? IsServer : -1);

        int32_t LevelInitialized = -1;
        int32_t LevelInitializedSites = 0;
        for (const uintptr_t Site : CreateChannel && SetChannelActor ? Finder::CallsTo(CreateChannel, false) : std::vector<uintptr_t>())
        {
            const uintptr_t Function = Finder::FunctionStart(Site);
            const uintptr_t FunctionEnd = Finder::FunctionEnd(Function);
            if (!Function || Function == DemoReplicate || FunctionEnd <= Function || !CallWithin(Function, FunctionEnd - Function, SetChannelActor))
            {
                continue;
            }
            uintptr_t Nearest = 0;
            for (uintptr_t Call = FindVirtualCall(Site - 0x20, Site); Call; Call = FindVirtualCall(Call + 1, Site))
            {
                Nearest = Call;
            }
            if (Nearest)
            {
                LevelInitialized = VirtualSlot(Nearest);
                ++LevelInitializedSites;
            }
        }
        const uintptr_t LevelInitializedBody = LevelInitializedSites == 1 ? VTableEntry(NetDriverVTable, LevelInitialized) : 0;
        RequireSlot(L"UNetDriver::IsLevelInitializedForActor", Natives::UNetDriver_IsLevelInitializedForActorIndex, LevelInitializedBody ? LevelInitialized : -1);

        const int32_t ClientHasInitializedLevelFor = LevelInitializedBody ? VirtualSlot(FindVirtualCall(LevelInitializedBody, LevelInitializedBody + 0x40)) : -1;
        // cmp [Connection+ClientWorldPackageName], reg: 48 39 on 1.1 and 1.2, 49 39 on 1.7.2 (through r8).
        ptrdiff_t ClientWorldPackageName = 0;
        for (uintptr_t Compare = LevelInitializedBody ? FindInstruction(LevelInitializedBody, LevelInitializedBody + 0x40, { -1, 0x39 }) : 0; Compare && !ClientWorldPackageName; Compare = FindInstruction(Compare + 1, LevelInitializedBody + 0x40, { -1, 0x39 }))
        {
            if ((Byte(Compare) & 0xF8) == 0x48 && (Byte(Compare + 2) & 0xC0) == 0x80 && (Byte(Compare + 2) & 0x07) != 0x04)
            {
                ClientWorldPackageName = *reinterpret_cast<const int32_t*>(Compare + 3);
            }
        }
        RequireOffset(L"UNetConnection::ClientWorldPackageName", Natives::UNetConnection_ClientWorldPackageName, ClientWorldPackageName);

        // 1.7.2's Describe adds ", UniqueId: %s"; each spelling exists in one build family only.
        uintptr_t Describe = 0;
        for (const wchar_t* Anchor : { L"[UNetConnection] RemoteAddr: %s, Name: %s, Driver: %s, IsServer: %s, PC: %s, Owner: %s",
                                       L"[UNetConnection] RemoteAddr: %s, Name: %s, Driver: %s, IsServer: %s, PC: %s, Owner: %s, UniqueId: %s" })
        {
            Describe = Describe ? Describe : FunctionOf(Anchor);
        }
        const uintptr_t DescribeEnd = Finder::FunctionEnd(Describe);
        void** ConnectionVTable = DescribeEnd ? CalledSlot(Describe, DescribeEnd, FunctionOf(L"UNetConnection::LowLevelGetRemoteAddress")).VTable : nullptr;

        // IsNetReady is the slot six before InitRemoteConnection and Tick the one before it; UNetDriver::TickFlush ticks
        // the server connection and each client connection through Tick. 1.7.2's TickFlush also calls IsNetReady(false)
        // on each first (slot 0x51, then Tick 0x50), so the calls aren't all one slot.
        const int32_t InitRemoteConnection = Finder::VTableIndex(ConnectionVTable, reinterpret_cast<void*>(FunctionOf(L"UNetConnection::InitRemoteConnection")), 0x400);
        const int32_t IsNetReady = InitRemoteConnection - 6;
        const int32_t ConnectionTick = IsNetReady - 1;
        int32_t ConnectionTicks = 0;
        for (uintptr_t Call = TickFlushEnd && ConnectionTick >= 0 ? FindInstruction(TickFlush, TickFlushEnd, { 0x48, 0x8B, 0x01, 0xFF, 0x90 }) : 0; Call; Call = FindInstruction(Call + 1, TickFlushEnd, { 0x48, 0x8B, 0x01, 0xFF, 0x90 }))
        {
            ConnectionTicks += VirtualSlot(Call + 3) == ConnectionTick ? 1 : 0;
        }
        const uintptr_t ConnectionTickBody = ConnectionTicks >= 2 ? VTableEntry(ConnectionVTable, ConnectionTick) : 0;
        RequireSlot(L"UNetConnection::Tick", Natives::UNetConnection_TickIndex, ConnectionTickBody ? ConnectionTick : -1);

        const uintptr_t IsNetReadyBody = InitRemoteConnection >= 6 && IsNetReady == ConnectionTick + 1 ? VTableEntry(ConnectionVTable, IsNetReady) : 0;
        const bool bIsNetReady = IsNetReadyBody && FindInstruction(IsNetReadyBody, IsNetReadyBody + 18, { 0x84, 0xD2, 0x74, -1, 0x8B, 0x81, -1, -1, 0x00, 0x00, 0xF7, 0xD8, 0x89, 0x81, -1, -1, 0x00, 0x00 }) == IsNetReadyBody;
        RequireSlot(L"UNetConnection::IsNetReady", Natives::UNetConnection_IsNetReadyIndex, bIsNetReady ? IsNetReady : -1);
        RequireOffset(L"UNetConnection::QueuedBits", Natives::UNetConnection_QueuedBits, bIsNetReady ? *reinterpret_cast<const int32_t*>(IsNetReadyBody + 14) : 0);
        RequireSlot(L"UNetConnection::FlushDormancy", Natives::UNetConnection_FlushDormancyIndex, Finder::VTableIndex(ConnectionVTable, reinterpret_cast<void*>(FunctionOf(L"FlushDormancy: %s. Connection: %s")), 0x400));

        const uintptr_t LevelFor = VTableEntry(ConnectionVTable, ClientHasInitializedLevelFor);
        const uintptr_t LevelForEnd = Finder::FunctionEnd(LevelFor);
        const ptrdiff_t VisibleLevelNames = ClientWorldPackageName + 8;
        bool bVisibleLevelData = false;
        bool bVisibleLevelCount = false;
        for (uintptr_t Load = LevelFor; ClientWorldPackageName && LevelForEnd && Load + 7 <= LevelForEnd; ++Load)
        {
            if ((Byte(Load) & 0xF8) != 0x48 || (Byte(Load + 2) & 0xC0) != 0x80 || (Byte(Load + 2) & 0x07) == 0x04)
            {
                continue;
            }
            const ptrdiff_t Offset = *reinterpret_cast<const int32_t*>(Load + 3);
            bVisibleLevelData = bVisibleLevelData || (Byte(Load + 1) == 0x8B && Offset == VisibleLevelNames);
            bVisibleLevelCount = bVisibleLevelCount || (Byte(Load + 1) == 0x63 && Offset == VisibleLevelNames + 8);
        }
        // 1.7.2 keeps ClientVisibleLevelNames as a TSet<FName>: the count is Elements.Num (+0x08) minus NumFreeIndices
        // (+0x34), read as mov eax, [conn+VLN+8]; sub eax, [conn+VLN+0x34] (connection slot 0x5E, 0x22243C0).
        const int32_t SetNum = static_cast<int32_t>(VisibleLevelNames + 0x08);
        const int32_t SetFree = static_cast<int32_t>(VisibleLevelNames + 0x34);
        const bool bVisibleLevelSet = ClientWorldPackageName && LevelForEnd && FindInstruction(LevelFor, LevelForEnd,
            { 0x8B, -1, SetNum & 0xFF, (SetNum >> 8) & 0xFF, (SetNum >> 16) & 0xFF, (SetNum >> 24) & 0xFF,
              0x2B, -1, SetFree & 0xFF, (SetFree >> 8) & 0xFF, (SetFree >> 16) & 0xFF, (SetFree >> 24) & 0xFF });
        const bool bVisibleLevels = bVisibleLevelData && (bVisibleLevelCount || bVisibleLevelSet);
        Natives::UNetConnection_ClientVisibleLevelNamesIsSet = bVisibleLevels && !bVisibleLevelCount;
        RequireSlot(L"UNetConnection::ClientHasInitializedLevelFor", Natives::UNetConnection_ClientHasInitializedLevelForIndex, bVisibleLevels ? ClientHasInitializedLevelFor : -1);
        RequireOffset(L"UNetConnection::ClientVisibleLevelNames", Natives::UNetConnection_ClientVisibleLevelNames, bVisibleLevels ? VisibleLevelNames : 0);

        const uintptr_t ConnectionTickEnd = Finder::FunctionEnd(ConnectionTickBody);
        ptrdiff_t TimeSensitive = 0;
        int32_t TimeSensitiveTests = 0;
        for (uintptr_t Test = ConnectionTickEnd ? FindInstruction(ConnectionTickBody, ConnectionTickEnd, { 0x80, -1, -1, -1, 0x00, 0x00, 0x00, 0x75, -1, 0x48, 0x8B, -1, -1, 0xF3, 0x0F, 0x10 }) : 0; Test; Test = FindInstruction(Test + 1, ConnectionTickEnd, { 0x80, -1, -1, -1, 0x00, 0x00, 0x00, 0x75, -1, 0x48, 0x8B, -1, -1, 0xF3, 0x0F, 0x10 }))
        {
            if ((Byte(Test + 1) & 0xF8) == 0xB8 && (Byte(Test + 1) & 0x07) != 0x04)
            {
                TimeSensitive = *reinterpret_cast<const int32_t*>(Test + 2);
                ++TimeSensitiveTests;
            }
        }
        RequireOffset(L"UNetConnection::TimeSensitive", Natives::UNetConnection_TimeSensitive, TimeSensitiveTests == 1 ? TimeSensitive : 0);

        uintptr_t TickCountTest = 0;
        int32_t TickCountTests = 0;
        for (uintptr_t Load = NetViewerEnd ? FindInstruction(NetViewer, NetViewerEnd, { 0x8B, -1, -1, -1, 0x00, 0x00, 0xA8, 0x01 }) : 0; Load; Load = FindInstruction(Load + 1, NetViewerEnd, { 0x8B, -1, -1, -1, 0x00, 0x00, 0xA8, 0x01 }))
        {
            if ((Byte(Load + 1) & 0xC0) == 0x80 && (Byte(Load + 1) & 0x07) != 0x04)
            {
                TickCountTest = Load;
                ++TickCountTests;
            }
        }
        RequireOffset(L"UNetConnection::TickCount", Natives::UNetConnection_TickCount, TickCountTests == 1 ? *reinterpret_cast<const int32_t*>(TickCountTest + 2) : 0);

        const uintptr_t GuidCacheLoad = CreateDestructionInfo ? FindInstruction(CreateDestructionInfo, CreateDestructionInfo + 0x40, { 0x48, 0x8B, 0x89 }) : 0;
        const uintptr_t AssignGuid = GuidCacheLoad ? FindRawCall(GuidCacheLoad + 7, GuidCacheLoad + 0x20) : 0;
        const uintptr_t DestroyedActorsBase = AssignGuid ? FindInstruction(AssignGuid + 5, AssignGuid + 0x10, { 0x48, 0x81 }) : 0;
        RequireOffset(L"UNetDriver::GuidCache", Natives::UNetDriver_GuidCache, GuidCacheLoad ? *reinterpret_cast<const int32_t*>(GuidCacheLoad + 3) : 0);
        RequireOffset(L"UNetDriver::DestroyedStartupOrDormantActors", Natives::UNetDriver_DestroyedStartupOrDormantActors, DestroyedActorsBase && (Byte(DestroyedActorsBase + 2) & 0xF8) == 0xC0 ? *reinterpret_cast<const int32_t*>(DestroyedActorsBase + 3) : 0);

        const uintptr_t ReplicateProperties = FunctionOf(L"ReplicateProperties: Object == NULL");
        const uintptr_t ReplicatePropertiesEnd = Finder::FunctionEnd(ReplicateProperties);
        ptrdiff_t ReplicationFrame = 0;
        int32_t ReplicationFrameReads = 0;
        for (uintptr_t Load = ReplicatePropertiesEnd ? FindInstruction(ReplicateProperties, ReplicatePropertiesEnd, { 0x44, 0x8B }) : 0; Load; Load = FindInstruction(Load + 1, ReplicatePropertiesEnd, { 0x44, 0x8B }))
        {
            if ((Byte(Load + 2) & 0xF8) == 0x80 && (Byte(Load + 2) & 0x07) != 0x04 && FindRawCall(Load + 7, Load + 0x10))
            {
                ReplicationFrame = *reinterpret_cast<const int32_t*>(Load + 3);
                ++ReplicationFrameReads;
            }
        }
        RequireOffset(L"UNetDriver::ReplicationFrame", Natives::UNetDriver_ReplicationFrame, ReplicationFrameReads == 1 ? ReplicationFrame : 0);

        const uintptr_t DumpRelevantRef = OnlyRef(L"net.DumpRelevantActors");
        const uintptr_t DumpRelevantInit = Finder::FunctionStart(DumpRelevantRef);
        ptrdiff_t DebugRelevantActors = 0;
        for (uintptr_t Lea = DumpRelevantInit ? FindInstruction(DumpRelevantInit, DumpRelevantRef, { 0x48, 0x8D, 0x05 }) : 0; Lea; Lea = FindInstruction(Lea + 1, DumpRelevantRef, { 0x48, 0x8D, 0x05 }))
        {
            const uintptr_t Handler = Finder::RelativeTarget(Lea, 3, 7);
            const uintptr_t Store = IsInText(Handler) ? FindInstruction(Handler, Handler + 0x20, { 0xC6, 0x80, -1, -1, 0x00, 0x00, 0x01 }) : 0;
            if (Store)
            {
                DebugRelevantActors = *reinterpret_cast<const int32_t*>(Store + 2);
            }
        }
        RequireOffset(L"UNetDriver::DebugRelevantActors", Natives::UNetDriver_DebugRelevantActors, DebugRelevantActors);

        const uintptr_t Standby = FunctionOf(L"Standby check is %s with RxTime (%f), TxTime (%f), PingThreshold (%d), JoinInProgressStandbyWaitTime (%f)");
        const uintptr_t StandbyEnd = Finder::FunctionEnd(Standby);
        ptrdiff_t LastStandbyField = 0;
        for (uintptr_t Copy = StandbyEnd ? FindInstruction(Standby, StandbyEnd, { 0x8B, -1, -1, -1, 0x00, 0x00, 0x89, -1, -1, -1, 0x00, 0x00 }) : 0; Copy; Copy = FindInstruction(Copy + 1, StandbyEnd, { 0x8B, -1, -1, -1, 0x00, 0x00, 0x89, -1, -1, -1, 0x00, 0x00 }))
        {
            if ((Byte(Copy + 1) & 0xC0) == 0x80 && (Byte(Copy + 7) & 0xC0) == 0x80 && (Byte(Copy + 7) & 0x07) != 0x04)
            {
                LastStandbyField = (std::max)(LastStandbyField, static_cast<ptrdiff_t>(*reinterpret_cast<const int32_t*>(Copy + 8)));
            }
        }
        const ptrdiff_t NetTag = DebugRelevantActors - static_cast<ptrdiff_t>(sizeof(int32_t));
        RequireOffset(L"UNetDriver::NetTag", Natives::UNetDriver_NetTag, LastStandbyField && NetTag == LastStandbyField + static_cast<ptrdiff_t>(sizeof(float)) ? NetTag : 0);

        const std::vector<std::pair<uintptr_t, uintptr_t>> VelocityThunks = NativeThunks("GetVelocity");
        const std::vector<std::pair<uintptr_t, uintptr_t>> ForceUpdateThunks = NativeThunks("ForceNetUpdate");
        const std::vector<std::pair<uintptr_t, uintptr_t>> FlushDormancyThunks = NativeThunks("FlushNetDormancy");
        const bool bActorThunks = VelocityThunks.size() == 1 && ForceUpdateThunks.size() == 1 && FlushDormancyThunks.size() == 1 && VelocityThunks[0].first
            && VelocityThunks[0].first == ForceUpdateThunks[0].first && VelocityThunks[0].first == FlushDormancyThunks[0].first;

        const int32_t Velocity = bActorThunks ? VirtualSlot(FindVirtualCall(VelocityThunks[0].second, VelocityThunks[0].second + 0x40)) : -1;
        const int32_t ViewerVelocity = TickCountTests == 1 ? VirtualSlot(FindVirtualCall(TickCountTest + 8, TickCountTest + 0x80)) : -1;
        RequireSlot(L"AActor::GetVelocity", Natives::AActor_GetVelocityIndex, Velocity == ViewerVelocity ? Velocity : -1);
        RequireSlot(L"AActor::ForceNetUpdate", Natives::AActor_ForceNetUpdateIndex, bActorThunks ? VirtualSlot(FindVirtualCall(ForceUpdateThunks[0].second, ForceUpdateThunks[0].second + 0x40)) : -1);

        uintptr_t FlushDormancy = 0;
        for (uintptr_t Jump = bActorThunks ? FlushDormancyThunks[0].second : 0; Jump && Jump + 5 <= FlushDormancyThunks[0].second + 0x30 && !FlushDormancy; ++Jump)
        {
            const uintptr_t Target = Finder::RelativeTarget(Jump, 1, 5);
            if ((Byte(Jump) == 0xE8 || Byte(Jump) == 0xE9) && IsInText(Target) && Finder::FunctionStart(Target) == Target)
            {
                FlushDormancy = Target;
            }
        }
        Require(L"AActor::FlushNetDormancy", Natives::AActor_FlushNetDormancy, FlushDormancy);

        const std::vector<std::pair<uintptr_t, uintptr_t>> PossessThunks = NativeThunks("Possess");
        uintptr_t ViewTargetThunk = 0;
        int32_t ViewTargetThunkCount = 0;
        for (const auto& [Registrar, Thunk] : PossessThunks.size() == 1 ? NativeThunks("GetViewTarget") : std::vector<std::pair<uintptr_t, uintptr_t>>())
        {
            if (Registrar && Registrar == PossessThunks[0].first)
            {
                ViewTargetThunk = Thunk;
                ++ViewTargetThunkCount;
            }
        }
        RequireSlot(L"AController::GetViewTarget", Natives::AController_GetViewTargetIndex, ViewTargetThunkCount == 1 ? VirtualSlot(FindVirtualCall(ViewTargetThunk, ViewTargetThunk + 0x40)) : -1);

        int32_t NetRelevant = -1;
        size_t NetRelevantSites = 0;
        for (const uintptr_t Site : NetViewer ? Finder::CallsTo(NetViewer, false) : std::vector<uintptr_t>())
        {
            const int32_t Slot = VirtualSlot(FindVirtualCall(Site + 5, Site + 0x40));
            if (Slot < 0 || (NetRelevantSites && Slot != NetRelevant))
            {
                NetRelevantSites = 0;
                break;
            }
            NetRelevant = Slot;
            ++NetRelevantSites;
        }
        RequireSlot(L"AActor::IsNetRelevantFor", Natives::AActor_IsNetRelevantForIndex, NetRelevantSites ? NetRelevant : -1);

        const uintptr_t FlushDormancyEnd = Finder::FunctionEnd(FlushDormancy);
        const uintptr_t ClientCheck = FlushDormancy ? FindInstruction(FlushDormancy, FlushDormancy + 0x60, { 0x83, 0xF8, 0x03 }) : 0;
        const uintptr_t DormancyLoad = ClientCheck ? FindInstruction(ClientCheck, ClientCheck + 0x10, { 0x0F, 0xB6 }) : 0;
        const ptrdiff_t NetDormancy = DormancyLoad && (Byte(DormancyLoad + 2) & 0xC0) == 0x80 && (Byte(DormancyLoad + 2) & 0x07) != 0x04 ? *reinterpret_cast<const int32_t*>(DormancyLoad + 3) : 0;
        RequireOffset(L"AActor::NetDormancy", Natives::AActor_NetDormancy, NetDormancy);

        const uintptr_t WorldCall = FlushDormancyEnd ? FindVirtualCall(FlushDormancy, FlushDormancyEnd) : 0;
        const uintptr_t AddActorCall = WorldCall ? FindRawCall(WorldCall + 6, FlushDormancyEnd) : 0;
        const uintptr_t AddNetworkActor = AddActorCall ? Finder::RelativeTarget(AddActorCall, 1, 5) : 0;
        const uintptr_t NetDriverCall = AddActorCall ? FindRawCall(AddActorCall + 5, FlushDormancyEnd) : 0;
        const uintptr_t GetNetDriver = NetDriverCall ? Finder::RelativeTarget(NetDriverCall, 1, 5) : 0;
        const uintptr_t FindNamedNetDriver = reinterpret_cast<uintptr_t>(Natives::UEngine_FindNamedNetDriver);
        bool bGetNetDriver = false;
        for (uintptr_t Jump = GetNetDriver; GetNetDriver && FindNamedNetDriver && Jump + 5 <= GetNetDriver + 0x60 && !bGetNetDriver; ++Jump)
        {
            bGetNetDriver = (Byte(Jump) == 0xE8 || Byte(Jump) == 0xE9) && Finder::RelativeTarget(Jump, 1, 5) == FindNamedNetDriver;
        }
        Require(L"UWorld::AddNetworkActor", Natives::UWorld_AddNetworkActor, AddNetworkActor);
        Require(L"AActor::GetNetDriver", Natives::AActor_GetNetDriver, bGetNetDriver ? GetNetDriver : 0);

        uintptr_t SetDormancy = 0;
        int32_t SetDormancyFunctions = 0;
        for (const uintptr_t Site : AddNetworkActor && FindNamedNetDriver && NetDormancy ? Finder::CallsTo(AddNetworkActor, false) : std::vector<uintptr_t>())
        {
            const uintptr_t Function = Finder::FunctionStart(Site);
            const uintptr_t FunctionEnd = Finder::FunctionEnd(Function);
            if (!Function || FunctionEnd <= Function || Function == SetDormancy || !CallWithin(Function, FunctionEnd - Function, FindNamedNetDriver))
            {
                continue;
            }
            for (uintptr_t Store = Function; Store + 6 <= FunctionEnd; ++Store)
            {
                if (Byte(Store) == 0x88 && (Byte(Store + 1) & 0xC0) == 0x80 && (Byte(Store + 1) & 0x07) != 0x04 && *reinterpret_cast<const int32_t*>(Store + 2) == NetDormancy)
                {
                    SetDormancy = Function;
                    ++SetDormancyFunctions;
                    break;
                }
            }
        }
        Require(L"AActor::SetNetDormancy", Natives::AActor_SetNetDormancy, SetDormancyFunctions == 1 ? SetDormancy : 0);

        ptrdiff_t TimeSeconds = 0;
        int32_t TimeSecondsReads = 0;
        for (const uintptr_t Site : FlushDormancy && NetDormancy ? Finder::CallsTo(FlushDormancy, false) : std::vector<uintptr_t>())
        {
            const uintptr_t Function = Finder::FunctionStart(Site);
            const uintptr_t DormancyTest = Function && Site - Function <= 0x20 ? FindInstruction(Function, Site, { 0x80, -1, static_cast<int32_t>(NetDormancy & 0xFF), static_cast<int32_t>((NetDormancy >> 8) & 0xFF), 0x00, 0x00, 0x01 }) : 0;
            if (!DormancyTest || (Byte(DormancyTest + 1) & 0xF8) != 0xB8)
            {
                continue;
            }
            const uintptr_t Load = FindInstruction(Site, Site + 0x30, { 0xF3, 0x0F, 0x10 });
            if (Load && (Byte(Load + 3) & 0xC0) == 0x80 && (Byte(Load + 3) & 0x07) != 0x04 && FindInstruction(Load + 8, Load + 0x18, { 0xF3, 0x0F, 0x5C }))
            {
                TimeSeconds = *reinterpret_cast<const int32_t*>(Load + 4);
                ++TimeSecondsReads;
            }
        }
        RequireOffset(L"UWorld::TimeSeconds", Natives::UWorld_TimeSeconds, TimeSecondsReads == 1 ? TimeSeconds : 0);

        ptrdiff_t ActorInitialized = 0;
        uint32_t ActorInitializedMask = 0;
        bool bActorInitializedAgree = true;
        for (const uintptr_t Ref : Finder::StringRefs(L"%s failed to route PostInitializeComponents.  Please call Super::PostInitializeComponents() in your <className>::PostInitializeComponents() function. "))
        {
            const uintptr_t Test = FindBackward(Ref, 0x60, { 0xF6, -1, -1, -1, 0x00, 0x00, -1, 0x0F, 0x85 });
            if (!Test || (Byte(Test + 1) & 0xF8) != 0x80 || (Byte(Test + 1) & 0x07) == 0x04)
            {
                continue;
            }
            const ptrdiff_t Offset = *reinterpret_cast<const int32_t*>(Test + 2);
            bActorInitializedAgree = bActorInitializedAgree && (!ActorInitialized || (ActorInitialized == Offset && ActorInitializedMask == Byte(Test + 6)));
            ActorInitialized = Offset;
            ActorInitializedMask = Byte(Test + 6);
        }
        RequireOffset(L"AActor::bActorInitialized", Natives::AActor_bActorInitialized, bActorInitializedAgree ? ActorInitialized : 0);
        Natives::AActor_bActorInitializedMask = Natives::AActor_bActorInitialized > 0 ? ActorInitializedMask : 0;

        ptrdiff_t AssociatingLevel = 0;
        uint32_t AssociatingLevelMask = 0;
        int32_t AssociatingLevelTests = 0;
        const uintptr_t TextEnd = Finder::TextEnd();
        for (uintptr_t Test = FindInstruction(Finder::TextBegin(), TextEnd, { 0xF6, -1, -1, -1, 0x00, 0x00, -1, 0x75, -1, 0xF6 }); Test; Test = FindInstruction(Test + 1, TextEnd, { 0xF6, -1, -1, -1, 0x00, 0x00, -1, 0x75, -1, 0xF6 }))
        {
            if ((Byte(Test + 1) & 0xF8) == 0x80 && (Byte(Test + 1) & 0x07) != 0x04 && Byte(Test + 10) == Byte(Test + 1) && Byte(Test + 16) == 0x75
                && *reinterpret_cast<const int32_t*>(Test + 11) == *reinterpret_cast<const int32_t*>(Test + 2) + 1)
            {
                AssociatingLevel = *reinterpret_cast<const int32_t*>(Test + 11);
                AssociatingLevelMask = Byte(Test + 15);
                ++AssociatingLevelTests;
            }
        }
        RequireOffset(L"ULevel::bIsAssociatingLevel", Natives::ULevel_bIsAssociatingLevel, AssociatingLevelTests == 1 ? AssociatingLevel : 0);
        Natives::ULevel_bIsAssociatingLevelMask = Natives::ULevel_bIsAssociatingLevel > 0 ? AssociatingLevelMask : 0;

        const uintptr_t AdaptiveRef = OnlyRef(L"net.UseAdaptiveNetUpdateFrequency");
        const uintptr_t AsVariableInt = AdaptiveRef ? FindInstruction(AdaptiveRef, AdaptiveRef + 0x40, { 0xFF, 0x52, 0x30 }) : 0;
        const uintptr_t RefStore = AsVariableInt ? FindInstruction(AsVariableInt, AsVariableInt + 0x10, { 0x48, 0x89, 0x05 }) : 0;
        const uintptr_t AdaptiveValue = RefStore ? Finder::RelativeTarget(RefStore, 3, 7) : 0;
        uintptr_t Adaptive = 0;
        int32_t AdaptiveReaders = 0;
        for (uintptr_t Load = Finder::TextBegin(); AdaptiveValue && Load + 7 <= TextEnd; ++Load)
        {
            if ((Byte(Load) & 0xFB) != 0x48 || Byte(Load + 1) != 0x8B || (Byte(Load + 2) & 0xC7) != 0x05 || Finder::RelativeTarget(Load, 3, 7) != AdaptiveValue)
            {
                continue;
            }
            const uintptr_t Function = Finder::FunctionStart(Load);
            const uintptr_t FunctionEnd = Finder::FunctionEnd(Function);
            if (Function && FunctionEnd > Function && FunctionEnd - Function <= 0x50 && Function != Adaptive)
            {
                Adaptive = Function;
                ++AdaptiveReaders;
            }
        }
        Require(L"UNetDriver::IsAdaptiveNetUpdateFrequencyEnabled", Natives::UNetDriver_IsAdaptiveNetUpdateFrequencyEnabled, AdaptiveReaders == 1 ? Adaptive : 0);

        const uintptr_t Engine = reinterpret_cast<uintptr_t>(Natives::GEngine);
        int32_t MaxTickRate = -1;
        int32_t MaxTickRateSites = 0;
        bool bMaxTickRateAgree = true;
        for (const wchar_t* Anchor : { L"Bringing %s up for play (max tick rate %i) at %s", L"Bringing preview %s up for play (max tick rate %i) at %s" })
        {
            for (const uintptr_t Ref : Finder::StringRefs(Anchor))
            {
                const uintptr_t Call = FindBackward(Ref, 0x80, { 0x48, 0x8B, 0x01, 0xFF, 0x90 });
                const uintptr_t EngineLoad = Call ? FindBackward(Call, 0x18, { 0x48, 0x8B, 0x0D }) : 0;
                if (!Engine || !EngineLoad || Finder::RelativeTarget(EngineLoad, 3, 7) != Engine)
                {
                    continue;
                }
                const int32_t Slot = VirtualSlot(Call + 3);
                bMaxTickRateAgree = bMaxTickRateAgree && (!MaxTickRateSites || Slot == MaxTickRate);
                MaxTickRate = Slot;
                ++MaxTickRateSites;
            }
        }
        RequireSlot(L"UEngine::GetMaxTickRate", Natives::UEngine_GetMaxTickRateIndex, bMaxTickRateAgree && MaxTickRateSites >= 2 ? MaxTickRate : -1);

        const uintptr_t CreateChannelEnd = Finder::FunctionEnd(CreateChannel);
        const bool bChannelLoop = CreateChannelEnd && FindInstruction(CreateChannel, CreateChannelEnd, { 0x48, 0x3D, 0x00, 0x28, 0x00, 0x00 });
        ptrdiff_t Channels = 0;
        for (uintptr_t Store = bChannelLoop ? FindInstruction(CreateChannel, CreateChannelEnd, { 0x48, 0x89 }) : 0; Store && !Channels; Store = FindInstruction(Store + 1, CreateChannelEnd, { 0x48, 0x89 }))
        {
            if ((Byte(Store + 2) & 0xC7) == 0x84 && (Byte(Store + 3) >> 6) == 3)
            {
                Channels = *reinterpret_cast<const int32_t*>(Store + 4);
            }
        }
        RequireOffset(L"UNetConnection::Channels", Natives::UNetConnection_Channels, Channels);

        ptrdiff_t IsPeer = 0;
        size_t IsPeerSites = 0;
        for (const uintptr_t Site : Finder::StringRefs(L"(PEER)"))
        {
            const bool bPeerTest = Byte(Site - 7) == 0x80 && (Byte(Site - 6) & 0xF8) == 0xB8 && (Byte(Site - 6) & 0x07) != 0x04 && Byte(Site - 1) == 0x00;
            const ptrdiff_t Offset = bPeerTest ? *reinterpret_cast<const int32_t*>(Site - 5) : 0;
            if (!Offset || (IsPeerSites && Offset != IsPeer))
            {
                IsPeerSites = 0;
                break;
            }
            IsPeer = Offset;
            ++IsPeerSites;
        }
        RequireOffset(L"UNetDriver::bIsPeer", Natives::UNetDriver_bIsPeer, IsPeerSites >= 2 ? IsPeer : 0);

        const ptrdiff_t DebugActorLists = (Natives::UNetDriver_DebugRelevantActors + 1 + 7) & ~static_cast<ptrdiff_t>(7);
        const bool bDebugActorLists = Natives::UNetDriver_DebugRelevantActors > 0 && DebugActorLists + 4 * 0x10 == Natives::UNetDriver_DestroyedStartupOrDormantActors;
        RequireOffset(L"UNetDriver::LastPrioritizedActors", Natives::UNetDriver_LastPrioritizedActors, bDebugActorLists ? DebugActorLists : 0);
        RequireOffset(L"UNetDriver::LastRelevantActors", Natives::UNetDriver_LastRelevantActors, bDebugActorLists ? DebugActorLists + 0x10 : 0);
        RequireOffset(L"UNetDriver::LastSentActors", Natives::UNetDriver_LastSentActors, bDebugActorLists ? DebugActorLists + 0x20 : 0);
        RequireOffset(L"UNetDriver::LastNonRelevantActors", Natives::UNetDriver_LastNonRelevantActors, bDebugActorLists ? DebugActorLists + 0x30 : 0);

        ptrdiff_t OwningChannel = 0;
        int32_t OwningChannelLoads = 0;
        for (uintptr_t Load = ReplicatePropertiesEnd && ActorChannelBits > 0 ? FindInstruction(ReplicateProperties, ReplicatePropertiesEnd, { 0x48, 0x8B }) : 0; Load; Load = FindInstruction(Load + 1, ReplicatePropertiesEnd, { 0x48, 0x8B }))
        {
            if ((Byte(Load + 2) & 0xF8) != 0x80 || (Byte(Load + 2) & 0x07) == 0x04)
            {
                continue;
            }
            const uintptr_t BitsRead = FindInstruction(Load + 7, Load + 0x30, { 0x44, 0x8B, 0x88, static_cast<int32_t>(ActorChannelBits & 0xFF), static_cast<int32_t>((ActorChannelBits >> 8) & 0xFF), 0x00, 0x00 });
            if (BitsRead)
            {
                OwningChannel = *reinterpret_cast<const int32_t*>(Load + 3);
                ++OwningChannelLoads;
            }
        }
        RequireOffset(L"FObjectReplicator::OwningChannel", Natives::FObjectReplicator_OwningChannel, OwningChannelLoads == 1 ? OwningChannel : 0);

        Sites::FlushActorDormancyCalls.clear();
        uintptr_t StrippedFlushDormancy = 0;
        bool bFlushDormancyCalls = AddNetworkActor != 0;
        for (const uintptr_t Function : { FlushDormancy, SetDormancyFunctions == 1 ? SetDormancy : uintptr_t(0) })
        {
            const uintptr_t End = Finder::FunctionEnd(Function);
            uintptr_t AddActorCall = 0;
            for (uintptr_t Call = End ? FindRawCall(Function, End) : 0; Call && !AddActorCall; Call = FindRawCall(Call + 5, End))
            {
                AddActorCall = Finder::RelativeTarget(Call, 1, 5) == AddNetworkActor ? Call : 0;
            }

            int32_t Calls = 0;
            for (uintptr_t Call = AddActorCall ? FindRawCall(AddActorCall + 5, End) : 0; Call; Call = FindRawCall(Call + 5, End))
            {
                const uintptr_t Target = Finder::RelativeTarget(Call, 1, 5);
                if (Byte(Target) == 0xC2 && Byte(Target + 1) == 0x00 && Byte(Target + 2) == 0x00 && (!StrippedFlushDormancy || Target == StrippedFlushDormancy))
                {
                    StrippedFlushDormancy = Target;
                    Sites::FlushActorDormancyCalls.push_back(reinterpret_cast<uint8_t*>(Call));
                    ++Calls;
                }
            }
            bFlushDormancyCalls = bFlushDormancyCalls && Calls == 2;
        }
        if (!bFlushDormancyCalls)
        {
            Log::Error(L"Finder: UNetDriver::FlushActorDormancy call sites not found ({})", Sites::FlushActorDormancyCalls.size());
            Sites::FlushActorDormancyCalls.clear();
            ++MissingCount;
        }
    }
}

namespace
{
    std::vector<void**> VTablesWithEntry(uintptr_t Function)
    {
        std::vector<void**> Entries;
        Memcury::PE::Section ReadOnly = Memcury::PE::Section::GetSection(".rdata");
        const uintptr_t Begin = ReadOnly.GetSectionStart().Get();
        const uintptr_t End = ReadOnly.GetSectionEnd().Get();
        for (uintptr_t At = (Begin + 7) & ~static_cast<uintptr_t>(7); Function && At + sizeof(uintptr_t) <= End; At += sizeof(uintptr_t))
        {
            if (*reinterpret_cast<const uintptr_t*>(At) == Function)
            {
                Entries.push_back(reinterpret_cast<void**>(At));
            }
        }
        return Entries;
    }

    bool IsFunctionEntry(uintptr_t Address)
    {
        if (!Address)
        {
            return false;
        }
        const uintptr_t Start = Finder::FunctionStart(Address);
        return Start == Address || (!Start && (Address & 0xF) == 0 && (Byte(Address - 1) == 0xCC || Byte(Address - 1) == 0xC3));
    }

    uintptr_t UniqueFunction(std::initializer_list<int32_t> Pattern)
    {
        uintptr_t Found = 0;
        int32_t Matches = 0;
        for (uintptr_t Site = FindInstruction(Finder::TextBegin(), Finder::TextEnd(), Pattern); Site; Site = FindInstruction(Site + 1, Finder::TextEnd(), Pattern))
        {
            Found = Site;
            ++Matches;
        }
        return Matches == 1 ? Found : 0;
    }

    int32_t DecodeGetWorldSlot(std::vector<uint8_t*>& GoalManagerCalls)
    {
        const uintptr_t ChoosePlayerStart = FunctionOf(L"ChoosePlayerStart: Could not find spot for Player! Spawning at engine PlayerStart!");
        const uintptr_t ChoosePlayerStartEnd = ChoosePlayerStart ? Finder::FunctionEnd(ChoosePlayerStart) : 0;
        const std::initializer_list<int32_t> GetCurrentOfWorld = { 0xFF, -1, -1, -1, -1, -1, 0x48, 0x8B, 0xC8, 0xE8 };
        uintptr_t GetCurrentStub = 0;
        int32_t GetWorldSlot = -1;
        for (uintptr_t Site = ChoosePlayerStartEnd ? FindInstruction(ChoosePlayerStart, ChoosePlayerStartEnd, GetCurrentOfWorld) : 0; Site; Site = FindInstruction(Site + 1, ChoosePlayerStartEnd, GetCurrentOfWorld))
        {
            const uintptr_t Call = Site + 9;
            const uintptr_t Target = Finder::RelativeTarget(Call, 1, 5);
            const int32_t Slot = (Byte(Site + 1) & 0xF8) == 0x90 && (Byte(Site + 1) & 0x07) != 0x04 ? VirtualSlot(Site) : -1;
            const bool bReturnsNull = Byte(Target) == 0x33 && Byte(Target + 1) == 0xC0 && Byte(Target + 2) == 0xC3;
            if (Slot >= 0 && bReturnsNull && (!GetCurrentStub || (Target == GetCurrentStub && Slot == GetWorldSlot)))
            {
                GetCurrentStub = Target;
                GetWorldSlot = Slot;
                GoalManagerCalls.push_back(reinterpret_cast<uint8_t*>(Call));
            }
        }
        return GetWorldSlot;
    }

    void ResolveZone()
    {
        const uintptr_t ZoneSpawnPawn = UniqueFunction({ 0x4D, 0x85, 0xC0, 0x74, 0x47, 0x48, 0x85, 0xD2, 0x74, 0x42, 0xE8 });
        Require(L"AFortGameModeZone::SpawnDefaultPawnFor_Implementation", Natives::AFortGameModeZone_SpawnDefaultPawnFor, ZoneSpawnPawn ? Finder::FunctionStart(ZoneSpawnPawn) : 0);
        Require(L"AFortGameMode::SpawnDefaultPawnFor_Implementation", Natives::AFortGameMode_SpawnDefaultPawnFor, FunctionOf(L"Couldn't spawn player pawn of type %s due to no start spot"));

        // or dword ptr [rdi+bWorldIsReady], 1; call [rax+GetWorld]; mov rcx, rax (bWorldIsReady +0x530 on 1.1 and 1.2,
        // +0x568 on 1.7.2; the plain or has dozens of matches, the call after it makes it unique).
        const uintptr_t WorldIsReady = UniqueFunction({ 0x83, 0x8F, -1, -1, 0x00, 0x00, 0x01, 0xFF, 0x90, -1, -1, 0x00, 0x00, 0x48, 0x8B, 0xC8 });
        Require(L"AFortGameMode::FinishWorldInitialization", Natives::AFortGameMode_FinishWorldInitialization, WorldIsReady ? Finder::FunctionStart(WorldIsReady) : 0);

        Require(L"AFortWorldManager::OnWorldRecordQueryComplete", Natives::AFortWorldManager_OnWorldRecordQueryComplete, FunctionOf(L"Invalid world record when transition to world query complete state!"));
        Require(L"AFortWorldManager::ServerTickLoadingOrCreatingWorld", Natives::AFortWorldManager_ServerTickLoadingOrCreatingWorld, FunctionOf(L"Skipping loading of level %s, LevelStreaming does not exist"));

        Sites::AIDirectorGetCurrentCalls.clear();
        for (const wchar_t* Text : { L"StartMissionAIEncounter: No AI Director!", L"StartMissionAIEncounterFromGeneratedProfile: No AI Director!", L"Failed to set simulated number of players, no AI Director found.", L"Failed to clear simulated number of players, no AI Director found." })
        {
            const uintptr_t Function = FunctionOf(Text);
            const uintptr_t FunctionEnd = Function ? Finder::FunctionEnd(Function) : 0;
            uintptr_t GetCurrentCall = 0;
            for (uintptr_t Call = FunctionEnd ? FindRawCall(Function, FunctionEnd) : 0; Call && !GetCurrentCall; Call = FindRawCall(Call + 1, FunctionEnd))
            {
                const uintptr_t Callee = Finder::RelativeTarget(Call, 1, 5);
                if (Byte(Callee) == 0x33 && Byte(Callee + 1) == 0xC0 && Byte(Callee + 2) == 0xC3)
                {
                    GetCurrentCall = Call;
                }
            }
            if (GetCurrentCall)
            {
                Sites::AIDirectorGetCurrentCalls.push_back(reinterpret_cast<uint8_t*>(GetCurrentCall));
            }
        }
        if (Sites::AIDirectorGetCurrentCalls.size() != 4)
        {
            ++MissingCount;
            Log::Error(L"Finder: AFortAIDirector::GetCurrent call sites: found {} of 4", Sites::AIDirectorGetCurrentCalls.size());
        }

        const uintptr_t ProfileFailed = OnlyRef(L"Failed to initialize profile for player: UniqueId:[%s] GameAccountId=[%s]");
        uintptr_t InitializeMcpProfile = 0;
        for (uintptr_t Call = ProfileFailed ? FindRawCall(ProfileFailed - 0x140, ProfileFailed) : 0; Call; Call = FindRawCall(Call + 1, ProfileFailed))
        {
            if (FindInstruction(Call + 5, Call + 0x10, { 0x84, 0xC0 }))
            {
                InitializeMcpProfile = Finder::RelativeTarget(Call, 1, 5);
            }
        }
        Require(L"AFortPlayerController::InitializeMcpProfile", Natives::AFortPlayerController_InitializeMcpProfile, InitializeMcpProfile);

        const uintptr_t UniqueIdToString = ProfileFailed ? FindBackward(ProfileFailed, 0x60, { 0xFF, 0x50, -1 }) : 0;
        Natives::FUniqueNetId_ToStringIndex = UniqueIdToString && Byte(UniqueIdToString + 2) % 8 == 0 ? Byte(UniqueIdToString + 2) / 8 : -1;
        if (Natives::FUniqueNetId_ToStringIndex < 0)
        {
            Log::Error(L"Finder: FUniqueNetId::ToString slot not found");
        }

        Require(L"AFortPlayerState::SetHeroType", Natives::AFortPlayerState_SetHeroType, FunctionOf(L"Hero gender does not match gender of HeroType %s!  Hero Gender is %d instead."));
        Require(L"UFortGlobals::ApplyHeroEffectsAndAbilities", Natives::UFortGlobals_ApplyHeroEffectsAndAbilities, FunctionOf(L"ApplyHeroEffectsAndAbilities succeeded"));

        const auto OnlyCaller = [](uintptr_t Function)
        {
            uintptr_t Caller = 0;
            for (uintptr_t Call : Function ? Finder::CallsTo(Function, false) : std::vector<uintptr_t>{})
            {
                const uintptr_t CallStart = Finder::FunctionStart(Call);
                if (!CallStart || (Caller && Caller != CallStart))
                {
                    return uintptr_t{ 0 };
                }
                Caller = CallStart;
            }
            return Caller;
        };
        const uintptr_t SetHero = OnlyCaller(reinterpret_cast<uintptr_t>(Natives::AFortPlayerState_SetHeroType));
        Require(L"AFortPlayerController::TrySetHeroImplementation", Natives::AFortPlayerController_TrySetHeroImplementation, OnlyCaller(SetHero));

        const uintptr_t ApplyHero = reinterpret_cast<uintptr_t>(Natives::UFortGlobals_ApplyHeroEffectsAndAbilities);
        const uintptr_t ApplyHeroEnd = ApplyHero ? Finder::FunctionEnd(ApplyHero) : 0;
        const std::initializer_list<int32_t> HasAllCall = { 0x49, 0x8B, 0xCE, 0xFF, 0x50 };
        uintptr_t TagAssetInterface = 0;
        uintptr_t HasAll = 0;
        for (uintptr_t Call = ApplyHeroEnd ? FindInstruction(ApplyHero, ApplyHeroEnd, HasAllCall) : 0; Call && !TagAssetInterface; Call = FindInstruction(Call + 1, ApplyHeroEnd, HasAllCall))
        {
            TagAssetInterface = FindBackward(Call, 0x20, { 0x4C, 0x8D, 0xB6 });
            HasAll = Call + 3;
        }
        const int32_t HasAllDisplacement = TagAssetInterface ? static_cast<int8_t>(Byte(HasAll + 2)) : 0;
        if (HasAllDisplacement > 0 && HasAllDisplacement % 8 == 0)
        {
            Natives::UAbilitySystemComponent_GameplayTagAssetInterface = *reinterpret_cast<const int32_t*>(TagAssetInterface + 3);
            Natives::IGameplayTagAssetInterface_HasAllMatchingGameplayTagsIndex = HasAllDisplacement / 8;
        }
        else
        {
            Log::Error(L"Finder: IGameplayTagAssetInterface::HasAllMatchingGameplayTags not found");
        }

        Sites::ChoosePlayerStartGoalManagerCalls.clear();
        std::vector<uint8_t*> GoalManagerCalls;
        RequireSlot(L"UObject::GetWorld", Natives::UObject_GetWorldIndex, DecodeGetWorldSlot(GoalManagerCalls));
        if (GoalManagerCalls.size() == 3)
        {
            Sites::ChoosePlayerStartGoalManagerCalls = std::move(GoalManagerCalls);
        }
        else
        {
            Log::Error(L"Finder: AFortGameModeZone::ChoosePlayerStart has {} AFortAIGoalManager::GetCurrent calls, expected 3", GoalManagerCalls.size());
            ++MissingCount;
        }

        const uintptr_t RecordLoaded = UniqueFunction({ 0x83, 0xA1, 0xE0, 0x03, 0x00, 0x00, 0xFD, 0x0F, 0xB6, 0xC2, 0x83, 0xE0, 0x01, 0x03, 0xC0, 0x09, 0x81, 0xE0, 0x03, 0x00, 0x00 });
        Require(L"AFortLobbyBeaconHost::SetWorldRecordLoaded", Natives::AFortLobbyBeaconHost_SetWorldRecordLoaded, IsFunctionEntry(RecordLoaded) ? RecordLoaded : 0);

        const uintptr_t FlushDormancy = reinterpret_cast<uintptr_t>(Natives::AActor_FlushNetDormancy);
        uintptr_t SharedEmpty = 0;
        for (uintptr_t Call = FlushDormancy ? FindRawCall(FlushDormancy, Finder::FunctionEnd(FlushDormancy)) : 0; Call && !SharedEmpty; Call = FindRawCall(Call + 5, Finder::FunctionEnd(FlushDormancy)))
        {
            const uintptr_t Target = Finder::RelativeTarget(Call, 1, 5);
            SharedEmpty = Byte(Target) == 0xC2 && Byte(Target + 1) == 0x00 && Byte(Target + 2) == 0x00 ? Target : 0;
        }
        Require(L"Shared empty function", Natives::SharedEmptyFunction, SharedEmpty);

        const uintptr_t Permissions = FunctionOf(L"e1f31c211f28413186262d37a13fc84d");
        const uintptr_t PermissionsEnd = Finder::FunctionEnd(Permissions);
        ptrdiff_t PermissionsArray = 0;
        int32_t PermissionAdds = 0;
        for (uintptr_t Lea = PermissionsEnd ? FindInstruction(Permissions, PermissionsEnd, { 0x49, 0x8D, 0x8D }) : 0; Lea; Lea = FindInstruction(Lea + 1, PermissionsEnd, { 0x49, 0x8D, 0x8D }))
        {
            const uintptr_t AddCall = FindRawCall(Lea + 7, Lea + 0x10);
            const ptrdiff_t Offset = *reinterpret_cast<const int32_t*>(Lea + 3);
            if (AddCall && (!PermissionAdds || Offset == PermissionsArray))
            {
                PermissionsArray = Offset;
                ++PermissionAdds;
            }
        }
        Require(L"FortMcpConfig service permissions", Natives::FortMcpConfig_UpdateServicePermissions, PermissionAdds >= 2 ? Permissions : 0);
        RequireOffset(L"FMcpServiceConfig::ServicePermissions", Natives::FMcpServiceConfig_ServicePermissions, PermissionAdds >= 2 ? PermissionsArray : 0);
    }

    void ResolveMcpProfile()
    {
        const uintptr_t SendRequestNow = FunctionOf(L"FATAL: Should not be possible to issue dedicated server commands without WITH_SERVER_CODE");
        const uintptr_t SendRequestNowEnd = SendRequestNow ? Finder::FunctionEnd(SendRequestNow) : 0;
        Require(L"UMcpProfileGroup::SendRequestNow", Natives::UMcpProfileGroup_SendRequestNow, SendRequestNowEnd ? SendRequestNow : 0);
        const auto Scan = [&](std::initializer_list<int32_t> Pattern)
        {
            return SendRequestNowEnd ? FindInstruction(SendRequestNow, SendRequestNowEnd, Pattern) : 0;
        };

        if (Scan({ 0x48, 0x83, 0x3A, 0x00 }))
        {
            Natives::FProfileHttpRequest_Profile = 0;
        }
        else
        {
            Log::Error(L"Finder: FProfileHttpRequest::Profile not found");
            ++MissingCount;
        }

        const uintptr_t HttpRequestCopy = Scan({ 0x48, 0x8B, 0x42, -1, 0x48, 0x89, 0x45, -1, 0x48, 0x8B, 0x42, -1 });
        const bool bHttpRequest = HttpRequestCopy && Byte(HttpRequestCopy + 11) == Byte(HttpRequestCopy + 3) + 8;
        RequireOffset(L"FProfileHttpRequest::HttpRequest", Natives::FProfileHttpRequest_HttpRequest, bHttpRequest ? Byte(HttpRequestCopy + 3) : 0);

        const uintptr_t PrepareCall = bHttpRequest ? FindVirtualCall(HttpRequestCopy, HttpRequestCopy + 0x40) : 0;
        RequireSlot(L"UMcpProfile::PrepareRequest", Natives::UMcpProfile_PrepareRequestIndex, VirtualSlot(PrepareCall));
        const uintptr_t ResponseTypeLoad = PrepareCall ? FindInstruction(HttpRequestCopy, PrepareCall, { 0x44, 0x8B, 0x42, -1 }) : 0;
        RequireOffset(L"FProfileHttpRequest::ResponseType", Natives::FProfileHttpRequest_ResponseType, ResponseTypeLoad ? Byte(ResponseTypeLoad + 3) : 0);

        const uintptr_t ServiceCopy = Scan({ 0x49, 0x8B, 0x46, -1, -1, -1, -1, 0x48, 0x8B, 0xB0, -1, -1, -1, -1, 0x4C, 0x8B, 0xB8, -1, -1, -1, -1 });
        const int32_t ServiceObject = ServiceCopy ? *reinterpret_cast<const int32_t*>(ServiceCopy + 17) : 0;
        const bool bService = ServiceCopy && *reinterpret_cast<const int32_t*>(ServiceCopy + 10) == ServiceObject + 8;
        RequireOffset(L"UMcpProfileGroup::McpSubsystem", Natives::UMcpProfileGroup_McpSubsystem, bService ? Byte(ServiceCopy + 3) : 0);
        RequireOffset(L"FOnlineSubsystemMcp::McpGameService", Natives::FOnlineSubsystemMcp_McpGameService, bService ? ServiceObject : 0);

        const uintptr_t ContextSwitch = Scan({ 0x8B, 0x4F, -1, 0x33, 0xDB, 0x48, 0x89, 0x5D, -1, 0x8B, 0xD3, 0x48, 0x89, 0x5D, -1, 0x85, 0xC9, 0x0F, 0x84 });
        RequireOffset(L"FProfileHttpRequest::ContextCredentials", Natives::FProfileHttpRequest_ContextCredentials, ContextSwitch ? Byte(ContextSwitch + 2) : 0);

        const uintptr_t CredentialsCheck = Scan({ 0x49, 0x8B, 0x8E, -1, -1, -1, -1, 0x48, 0x85, 0xC9, 0x0F, 0x84, -1, -1, -1, -1, 0x48, 0x8B, 0x01, 0xFF, 0x50, -1, 0x84, 0xC0 });
        const ptrdiff_t CredentialsNetId = CredentialsCheck ? *reinterpret_cast<const int32_t*>(CredentialsCheck + 3) : 0;
        RequireOffset(L"UMcpProfileGroup::CredentialsAccountId", Natives::UMcpProfileGroup_CredentialsAccountId, CredentialsNetId > static_cast<ptrdiff_t>(sizeof(void*)) ? CredentialsNetId - static_cast<ptrdiff_t>(sizeof(void*)) : 0);
        RequireSlot(L"FUniqueNetId::IsValid", Natives::FUniqueNetId_IsValidIndex, CredentialsCheck && Byte(CredentialsCheck + 21) % 8 == 0 ? Byte(CredentialsCheck + 21) / 8 : -1);
        const uintptr_t QueueForUser = CredentialsCheck ? FindRawCall(CredentialsCheck + 24, CredentialsCheck + 0x60) : 0;
        Require(L"FOnlineSubsystemMcp::QueueRequest (user)", Natives::FOnlineSubsystemMcp_QueueRequestForUser, QueueForUser ? Finder::RelativeTarget(QueueForUser, 1, 5) : 0);

        Require(L"UFortRegisteredPlayerInfo::OnAccountInventoryChanged", Natives::UFortRegisteredPlayerInfo_OnAccountInventoryChanged, FunctionOf(L"UFortRegisteredPlayerInfo::OnAccountInventoryChanged trying to initialize ability system actor"));

        const uintptr_t QueueForClient = FunctionOf(L"Queuing Mcp request for client=%s url=%s");
        Require(L"FOnlineSubsystemMcp::QueueRequest (client)", Natives::FOnlineSubsystemMcp_QueueRequestForClient, QueueForClient);

        const uintptr_t Dispatch = FunctionOf(L"UFortMcpResource: Failed to start mcp resource request. Error: %s");
        const uintptr_t DispatchEnd = Dispatch ? Finder::FunctionEnd(Dispatch) : 0;
        uintptr_t ClientCall = 0;
        for (uintptr_t Call = QueueForClient && DispatchEnd ? FindRawCall(Dispatch, DispatchEnd) : 0; Call && !ClientCall; Call = FindRawCall(Call + 1, DispatchEnd))
        {
            ClientCall = Finder::RelativeTarget(Call, 1, 5) == QueueForClient ? Call : 0;
        }
        const uintptr_t ConfigLea = ClientCall ? FindInstruction(Dispatch, ClientCall, { -1, 0x8D, -1, -1, 0x48, 0x8D, 0x15, -1, -1, -1, -1, 0xE8 }) : 0;
        const bool bConfigLea = ConfigLea && (Byte(ConfigLea + 2) & 0xF8) == 0x48 && (Byte(ConfigLea + 2) & 0x07) != 0x04;
        RequireOffset(L"FGameServiceMcp::ServiceConfig", Natives::FGameServiceMcp_ServiceConfig, bConfigLea ? Byte(ConfigLea + 3) : 0);
    }

    void ResolveThreadingForPerformance()
    {
        Sites::ShouldUseThreadingForPerformanceDedicatedCheck = nullptr;
        const uintptr_t OneThread = OnlyRef(L"ONETHREAD");
        const bool bParamTest = OneThread && Byte(OneThread + 7) == 0xE8 && Byte(OneThread + 12) == 0x84 && Byte(OneThread + 13) == 0xC0 && Byte(OneThread + 14) == 0x75;
        const uintptr_t OnlyOneThread = bParamTest ? OneThread + 16 + static_cast<int8_t>(Byte(OneThread + 15)) : 0;
        if (OnlyOneThread && Byte(OnlyOneThread) == 0xC6 && Byte(OnlyOneThread + 1) == 0x05 && Byte(OnlyOneThread + 6) == 0x01)
        {
            Sites::ShouldUseThreadingForPerformanceDedicatedCheck = reinterpret_cast<uint8_t*>(OneThread + 14);
        }
        else
        {
            Log::Error(L"Finder: FApp::ShouldUseThreadingForPerformance not found");
            ++MissingCount;
        }
    }

    void ResolveLinkerFilter()
    {
        Sites::LinkerFilterClientButNotServer = nullptr;
        const uintptr_t IsClient = reinterpret_cast<uintptr_t>(Natives::GIsClient);
        const uintptr_t IsServer = reinterpret_cast<uintptr_t>(Natives::GIsServer);
        const std::initializer_list<int32_t> FilterPattern = { 0x40, 0x38, -1, -1, -1, -1, -1, 0x75, -1, 0x40, 0x38, -1, -1, -1, -1, -1, 0x74, -1, 0xC6, -1, -1, -1, 0x00, 0x00, 0x01 };
        int32_t Filters = 0;
        for (uintptr_t Site = IsClient && IsServer ? FindInstruction(Finder::TextBegin(), Finder::TextEnd(), FilterPattern) : 0; Site; Site = FindInstruction(Site + 1, Finder::TextEnd(), FilterPattern))
        {
            const bool bReadsFlags = (Byte(Site + 2) & 0xC7) == 0x05 && Finder::RelativeTarget(Site, 3, 7) == IsClient
                && (Byte(Site + 11) & 0xC7) == 0x05 && Finder::RelativeTarget(Site + 9, 3, 7) == IsServer;
            const bool bStoresMember = (Byte(Site + 19) & 0xC0) == 0x80 && (Byte(Site + 19) & 0x38) == 0x00 && (Byte(Site + 19) & 0x07) != 0x04;
            if (bReadsFlags && bStoresMember)
            {
                Sites::LinkerFilterClientButNotServer = reinterpret_cast<uint8_t*>(Site + 18);
                ++Filters;
            }
        }
        if (Filters != 1)
        {
            Log::Error(L"Finder: FLinker::FLinker FilterClientButNotServer store not found ({} matches)", Filters);
            Sites::LinkerFilterClientButNotServer = nullptr;
            ++MissingCount;
        }
    }

    void ResolveActorTick()
    {
        const uintptr_t GetWorldSettings = reinterpret_cast<uintptr_t>(Natives::UWorld_GetWorldSettings);
        const std::initializer_list<int32_t> TickPattern = { 0x41, 0xB0, 0x01, 0x33, 0xD2, 0x48, 0x8B, 0xC8, 0xE8, -1, -1, -1, -1, 0x48, 0x85, 0xC0, 0x74, -1, 0x0F, 0x28, -1, 0x48, 0x8B, -1, 0xE8 };
        uintptr_t ReceiveTickCall = 0;
        int32_t ReceiveTickCalls = 0;
        for (uintptr_t Site = GetWorldSettings ? FindInstruction(Finder::TextBegin(), Finder::TextEnd(), TickPattern) : 0; Site; Site = FindInstruction(Site + 1, Finder::TextEnd(), TickPattern))
        {
            const uintptr_t Call = Site + 24;
            const uintptr_t ReceiveTick = Finder::RelativeTarget(Call, 1, 5);
            const bool bFloatThunk = FindInstruction(ReceiveTick, ReceiveTick + 6, { 0xF3, 0x0F, 0x11, 0x4C, 0x24, 0x10 }) == ReceiveTick;
            if (Finder::RelativeTarget(Site + 8, 1, 5) == GetWorldSettings && Byte(Site + 20) == 0xCE && bFloatThunk && Site - Finder::FunctionStart(Site) <= 0x40)
            {
                ReceiveTickCall = Call;
                ++ReceiveTickCalls;
            }
        }
        Sites::ActorTickReceiveTick = ReceiveTickCalls == 1 ? reinterpret_cast<uint8_t*>(ReceiveTickCall) : nullptr;
        Require(L"AActor::ReceiveTick", Natives::AActor_ReceiveTick, ReceiveTickCalls == 1 ? Finder::RelativeTarget(ReceiveTickCall, 1, 5) : 0);
    }

    void ResolveSpawnContext()
    {
        const uintptr_t SpawnRefusal = OnlyRef(L"Unable to spawn class '%s' due to client/server context.");
        uintptr_t CanCreate = 0;
        for (uintptr_t Call = SpawnRefusal ? FindRawCall(SpawnRefusal - 0x60, SpawnRefusal) : 0; Call && !CanCreate; Call = FindRawCall(Call + 5, SpawnRefusal))
        {
            const uintptr_t Target = Finder::RelativeTarget(Call, 1, 5);
            if (Byte(Call + 5) == 0x84 && Byte(Call + 6) == 0xC0 && FindInstruction(Target, Target + 10, { 0x48, 0x8B, 0x01, 0x48, 0xFF, 0xA0 }) == Target)
            {
                CanCreate = Target;
            }
        }
        Require(L"UObject::CanCreateInCurrentContext", Natives::UObject_CanCreateInCurrentContext, CanCreate);
        const int32_t ClientSlot = CanCreate ? *reinterpret_cast<const int32_t*>(CanCreate + 6) / static_cast<int32_t>(sizeof(void*)) : -1;
        RequireSlot(L"UObject::NeedsLoadForClient", Natives::UObject_NeedsLoadForClientIndex, ClientSlot);
        RequireSlot(L"UObject::NeedsLoadForServer", Natives::UObject_NeedsLoadForServerIndex, ClientSlot >= 0 ? ClientSlot + 1 : -1);

        Require(L"UWorld::SpawnActor", Natives::UWorld_SpawnActor, FunctionOf(L"SpawnActor failed because no class was specified"));
    }

    void ResolveAbilitySystem()
    {
        Require(L"UAbilitySystemComponent::InternalTryActivateAbility", Natives::UAbilitySystemComponent_InternalTryActivateAbility, FunctionOf(L"InternalTryActivateAbility called with invalid Handle! ASC: %s. AvatarActor: %s"));

        const uintptr_t ActivateOnce = FunctionOf(L"GiveAbilityAndActivateOnce called on ability %s on the client, not allowed!");
        const uintptr_t ActivateOnceEnd = ActivateOnce ? Finder::FunctionEnd(ActivateOnce) : 0;
        uintptr_t GiveAbility = 0;
        int32_t LockCount = 0;
        for (uintptr_t Call = ActivateOnceEnd ? FindRawCall(ActivateOnce, ActivateOnceEnd) : 0; Call && !GiveAbility; Call = FindRawCall(Call + 1, ActivateOnceEnd))
        {
            const uintptr_t Target = Finder::RelativeTarget(Call, 1, 5);
            if (const uintptr_t Compare = FindInstruction(Target, Target + 0x20, { 0x83, 0xB9, -1, -1, -1, -1, 0x00 }))
            {
                GiveAbility = Target;
                LockCount = *reinterpret_cast<const int32_t*>(Compare + 2);
            }
        }

        Natives::UAbilitySystemComponent_GiveAbility = GiveAbility;

        uintptr_t MarkDirty = 0;
        const uintptr_t GiveAbilityEnd = GiveAbility ? Finder::FunctionEnd(GiveAbility) : 0;
        for (uintptr_t Site = GiveAbilityEnd ? FindVirtualCall(GiveAbility, GiveAbilityEnd) : 0; Site && !MarkDirty; Site = FindVirtualCall(Site + 1, GiveAbilityEnd))
        {
            if (const uintptr_t Call = FindRawCall(Site + 6, Site + 0x18))
            {
                MarkDirty = Finder::RelativeTarget(Call, 1, 5);
            }
        }
        Require(L"UAbilitySystemComponent::MarkAbilitySpecDirty", Natives::UAbilitySystemComponent_MarkAbilitySpecDirty, MarkDirty);

        const uintptr_t MarkDirtyEnd = MarkDirty ? Finder::FunctionEnd(MarkDirty) : 0;
        const uintptr_t IncrementId = MarkDirtyEnd ? FindInstruction(MarkDirty, MarkDirtyEnd, { 0xFF, 0x41, -1 }) : 0;
        RequireOffset(L"FFastArraySerializer::IDCounter", Natives::FFastArraySerializer_IDCounter, IncrementId ? Byte(IncrementId + 2) : 0);
        const uintptr_t MarkArrayDirty = IncrementId ? FindRawCall(IncrementId, MarkDirtyEnd) : 0;
        Require(L"FFastArraySerializer::MarkArrayDirty", Natives::FFastArraySerializer_MarkArrayDirty, MarkArrayDirty ? Finder::RelativeTarget(MarkArrayDirty, 1, 5) : 0);

        const uint8_t* Count = reinterpret_cast<const uint8_t*>(&LockCount);
        const uintptr_t Increment = LockCount > 0 ? UniqueFunction({ 0xFF, 0x81, Count[0], Count[1], Count[2], Count[3], 0xC3 }) : 0;
        Require(L"UAbilitySystemComponent::IncrementAbilityListLock", Natives::UAbilitySystemComponent_IncrementAbilityListLock, IsFunctionEntry(Increment) ? Increment : 0);
        const uintptr_t Decrement = LockCount > 0 ? UniqueFunction({ 0x83, 0xA9, Count[0], Count[1], Count[2], Count[3], 0x01 }) : 0;
        Require(L"UAbilitySystemComponent::DecrementAbilityListLock", Natives::UAbilitySystemComponent_DecrementAbilityListLock, Decrement ? Finder::FunctionStart(Decrement) : 0);
    }

    void ResolveEngineLoopTicks()
    {
        Sites::ShouldUseIdleModeCheck = nullptr;
        Sites::ShouldUseIdleModeFalse = nullptr;
        const std::initializer_list<int32_t> IdleCheck = { 0x48, 0x8B, 0x05, -1, -1, -1, -1, 0x83, 0x38, 0x00, 0x74, -1 };
        for (const uintptr_t Ref : Finder::StringRefs(L"t.IdleWhenNotForeground"))
        {
            const uintptr_t Function = Finder::FunctionStart(Ref);
            const uintptr_t Check = Function ? FindInstruction(Function, Function + 0x40, IdleCheck) : 0;
            if (Check && !Sites::ShouldUseIdleModeCheck)
            {
                Sites::ShouldUseIdleModeCheck = reinterpret_cast<uint8_t*>(Check);
                Sites::ShouldUseIdleModeFalse = reinterpret_cast<uint8_t*>(Check + 12 + static_cast<int8_t>(Byte(Check + 11)));
            }
        }
        const uint8_t* IdleFalse = Sites::ShouldUseIdleModeFalse;
        if (!IdleFalse || IdleFalse[0] != 0x32 || IdleFalse[1] != 0xC0)
        {
            Log::Error(L"Finder: FEngineLoop::ShouldUseIdleMode not found");
            Sites::ShouldUseIdleModeCheck = nullptr;
            Sites::ShouldUseIdleModeFalse = nullptr;
            ++MissingCount;
        }

        Sites::MaxTickRateWait = {};
        const uintptr_t UpdateTime = Finder::FunctionStart(First(Finder::StringRefs(L"Detected negative delta time - on AMD systems please install http://files.aoaforums.com/I3199-setup.zip.html")));
        const uintptr_t UpdateTimeEnd = Finder::FunctionEnd(UpdateTime);
        const std::initializer_list<int32_t> SleepPattern = { 0x76, 0x10, 0xF3, 0x0F, 0x5C, -1, -1, -1, -1, -1, 0x0F, 0x28, -1, 0xE8 };
        const uintptr_t Sleep = UpdateTimeEnd ? FindInstruction(UpdateTime, UpdateTimeEnd, SleepPattern) : 0;
        const uintptr_t SleepTarget = Sleep ? Finder::RelativeTarget(Sleep + 13, 1, 5) : 0;
        const bool bSlack = Sleep && (Byte(Sleep + 5) & 0xC7) == 0x05 && *reinterpret_cast<const float*>(Finder::RelativeTarget(Sleep + 2, 4, 8)) == 0.002f;
        uintptr_t Spin = 0;
        for (uintptr_t Exit = bSlack ? FindInstruction(Sleep + 18, Sleep + 0x60, { 0x73, -1, 0x0F, 0x28, -1, 0xE8 }) : 0; Exit && !Spin; Exit = FindInstruction(Exit + 1, Sleep + 0x60, { 0x73, -1, 0x0F, 0x28, -1, 0xE8 }))
        {
            Spin = Finder::RelativeTarget(Exit + 5, 1, 5) == SleepTarget ? Exit : 0;
        }
        if (Spin)
        {
            Sites::MaxTickRateWait = { reinterpret_cast<uint8_t*>(Sleep), reinterpret_cast<uint8_t*>(Sleep + 2), reinterpret_cast<uint8_t*>(Spin) };
        }
        else
        {
            Log::Error(L"Finder: UEngine::UpdateTimeAndHandleMaxTickRate wait not found");
            ++MissingCount;
        }
    }

    void ResolveSession()
    {
        const uintptr_t FortInitGame = FunctionOf(L"playlistId");
        uintptr_t SessionClass = 0;
        size_t SessionClassEntries = 0;
        for (void** Entry : VTablesWithEntry(FortInitGame))
        {
            const uintptr_t Candidate = reinterpret_cast<uintptr_t>(Entry[2]);
            const bool bStaticClassGetter = IsInText(Candidate)
                && FindInstruction(Candidate, Candidate + 0x1A, { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xDA, 0xE8, -1, -1, -1, -1, 0x48, 0x89, 0x03, 0x48, 0x8B, 0xC3, 0x48, 0x83, 0xC4, 0x20, 0x5B, 0xC3 }) == Candidate;
            if (!bStaticClassGetter || (SessionClass && Candidate != SessionClass))
            {
                SessionClassEntries = 0;
                break;
            }
            SessionClass = Candidate;
            ++SessionClassEntries;
        }
        Require(L"AFortGameMode::GetGameSessionClass", Natives::AFortGameMode_GetGameSessionClass, SessionClassEntries ? SessionClass : 0);

        const uintptr_t PatchCheck = FunctionOf(L"No local player to perform check!");
        const uintptr_t PatchCheckEnd = Finder::FunctionEnd(PatchCheck);
        uintptr_t DedicatedInstance = 0;
        for (uintptr_t Call = PatchCheckEnd ? FindRawCall(PatchCheck, PatchCheckEnd) : 0; Call && !DedicatedInstance; Call = FindRawCall(Call + 5, PatchCheckEnd))
        {
            const uintptr_t Target = Finder::RelativeTarget(Call, 1, 5);
            if (FindInstruction(Target, Target + 0x11, { 0x48, 0x8B, 0x41, -1, 0x48, 0x85, 0xC0, 0x74, -1, 0x0F, 0xB6, 0x80, -1, -1, 0x00, 0x00, 0xC3 }) == Target)
            {
                DedicatedInstance = Target;
            }
        }
        Require(L"UGameInstance::IsDedicatedServerInstance", Natives::UGameInstance_IsDedicatedServerInstance, DedicatedInstance);
        const ptrdiff_t WorldContext = DedicatedInstance ? Byte(DedicatedInstance + 3) : 0;
        const ptrdiff_t RunAsDedicated = DedicatedInstance ? *reinterpret_cast<const int32_t*>(DedicatedInstance + 12) : 0;
        RequireOffset(L"UGameInstance::WorldContext", Natives::UGameInstance_WorldContext, WorldContext);
        RequireOffset(L"FWorldContext::RunAsDedicated", Natives::FWorldContext_RunAsDedicated, RunAsDedicated);

        uintptr_t StartCheck = 0;
        int32_t StartChecks = 0;
        const std::initializer_list<int32_t> StartPattern = { 0x48, 0x8B, 0x41, static_cast<int32_t>(WorldContext), 0x48, 0x8B, 0xD9, 0x48, 0x85, 0xC0, 0x74, -1, 0x80, 0xB8,
            static_cast<int32_t>(RunAsDedicated & 0xFF), static_cast<int32_t>((RunAsDedicated >> 8) & 0xFF), 0x00, 0x00, 0x00, 0x75, -1 };
        for (uintptr_t Site = RunAsDedicated ? FindInstruction(Finder::TextBegin(), Finder::TextEnd(), StartPattern) : 0; Site; Site = FindInstruction(Site + 1, Finder::TextEnd(), StartPattern))
        {
            StartCheck = Site;
            ++StartChecks;
        }
        uintptr_t StartBranch = StartChecks == 1 ? StartCheck + 10 : 0;
        uintptr_t StartTarget = StartBranch ? StartBranch + 11 + static_cast<int8_t>(Byte(StartBranch + 10)) : 0;
        bool bStartCheck = StartTarget && StartBranch + 2 + static_cast<int8_t>(Byte(StartBranch + 1)) < StartTarget && StartTarget - (StartBranch + 2) <= 0x7F;
        if (StartChecks == 0 && DedicatedInstance)
        {
            // 1.7.2 (0x593180) calls IsDedicatedServerInstance instead of inlining it, so the branch to skip is the jnz
            // after the call: call IsDedicatedServerInstance; test al, al; jnz Skip; mov rcx, [rbx+UpdateManager];
            // test rcx, rcx; jz Skip; mov rax, [rcx]; xor edx, edx; call [rax+...].
            const std::initializer_list<int32_t> CallPattern = { 0xE8, -1, -1, -1, -1, 0x84, 0xC0, 0x75, -1, 0x48, 0x8B, 0x8B, -1, -1, 0x00, 0x00,
                0x48, 0x85, 0xC9, 0x74, -1, 0x48, 0x8B, 0x01, 0x33, 0xD2, 0xFF, 0x90 };
            for (const uintptr_t Call : Finder::CallsTo(DedicatedInstance, false))
            {
                const uintptr_t Branch = Call + 7;
                const uintptr_t Target = Branch + 2 + static_cast<int8_t>(Byte(Branch + 1));
                if (FindInstruction(Call, Call + 28, CallPattern) == Call && Call + 21 + static_cast<int8_t>(Byte(Call + 20)) == Target)
                {
                    StartBranch = Branch;
                    StartTarget = Target;
                    ++StartChecks;
                }
            }
            bStartCheck = StartChecks == 1 && StartTarget > StartBranch + 2 && StartTarget - (StartBranch + 2) <= 0x7F;
        }
        Sites::FortGameInstanceStartDedicatedCheck = bStartCheck ? reinterpret_cast<uint8_t*>(StartBranch) : nullptr;
        Sites::FortGameInstanceStartDedicatedTarget = bStartCheck ? reinterpret_cast<uint8_t*>(StartTarget) : nullptr;
        if (!bStartCheck)
        {
            Log::Error(L"Finder: UFortGameInstance::StartGameInstance dedicated check not found ({} matches)", StartChecks);
            ++MissingCount;
        }

        uintptr_t IsDedicated = 0;
        int32_t IsDedicatedMatches = 0;
        const std::initializer_list<int32_t> IsDedicatedPattern = { 0x33, 0xC0, 0x38, 0x81, -1, -1, -1, -1, 0x0F, 0x95, 0xC0, 0xC3 };
        for (uintptr_t Getter = FindInstruction(Finder::TextBegin(), Finder::TextEnd(), IsDedicatedPattern); Getter; Getter = FindInstruction(Getter + 1, Finder::TextEnd(), IsDedicatedPattern))
        {
            const int32_t Offset = *reinterpret_cast<const int32_t*>(Getter + 4);
            for (void** Entry : VTablesWithEntry(Getter))
            {
                const uintptr_t Setter = reinterpret_cast<uintptr_t>(Entry[2]);
                if (IsInText(Setter) && Byte(Setter) == 0x88 && Byte(Setter + 1) == 0x91 && *reinterpret_cast<const int32_t*>(Setter + 2) == Offset && Byte(Setter + 6) == 0xC3)
                {
                    if (Getter != IsDedicated)
                    {
                        ++IsDedicatedMatches;
                    }
                    IsDedicated = Getter;
                }
            }
        }
        Require(L"FOnlineSubsystemImpl::IsDedicated", Natives::FOnlineSubsystemImpl_IsDedicated, IsDedicatedMatches == 1 ? IsDedicated : 0);
        RequireOffset(L"FOnlineSubsystemImpl::bForceDedicated", Natives::FOnlineSubsystemImpl_bForceDedicated, IsDedicatedMatches == 1 ? *reinterpret_cast<const int32_t*>(IsDedicated + 4) : 0);
    }
}

bool Finder::ResolveClient()
{
    MissingCount = 0;

    ResolveConsole();
    ResolveConsoleManagerGlobals();
    ResolveAutoCompleteSources();
    ResolveViewportClient();

    Log::Info(L"Finder: client sites resolved, {} missing", MissingCount);
    return MissingCount == 0;
}

bool Finder::ResolveObjectGetWorld()
{
    std::vector<uint8_t*> GoalManagerCalls;
    const int32_t GetWorldSlot = DecodeGetWorldSlot(GoalManagerCalls);
    if (GetWorldSlot < 0)
    {
        Log::Error(L"Finder: UObject::GetWorld slot not found");
        return false;
    }
    Natives::UObject_GetWorldIndex = GetWorldSlot;
    return true;
}

bool Finder::ResolveServer()
{
    MissingCount = 0;

    ResolveConsoleManagerGlobals();
    ResolveEngineNet();
    ResolveNetMode();
    ResolveWorldTravel();
    ResolveGameMapsSettings();
    ResolvePreInitServerPatch();
    ResolveDedicatedServerChecks();
    ResolveReplication();
    ResolveSession();
    ResolveEngineLoopTicks();
    ResolveSpawnContext();
    ResolveActorTick();
    ResolveLinkerFilter();
    ResolveThreadingForPerformance();
    ResolveZone();
    ResolveAbilitySystem();
    ResolveMcpProfile();

    Log::Info(L"Finder: server sites resolved, {} missing", MissingCount);
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

uintptr_t Finder::LogCategory(const wchar_t* CategoryName)
{
    const uintptr_t Constructor = reinterpret_cast<uintptr_t>(Natives::FLogCategoryBase_FLogCategoryBase);
    for (const uintptr_t Ref : Finder::StringRefs(CategoryName))
    {
        if (!Constructor || Finder::NextCall(Ref, 0x20) != Constructor)
        {
            continue;
        }
        if (const uintptr_t Lea = FindInstruction(Ref - 0x10, Ref + 0x10, { 0x48, 0x8D, 0x0D }))
        {
            return Finder::RelativeTarget(Lea, 3, 7);
        }
    }
    return 0;
}

bool Finder::ResolveConsoleManager()
{
    void** VTable = Natives::IConsoleManager_Singleton && *Natives::IConsoleManager_Singleton ? *reinterpret_cast<void***>(*Natives::IConsoleManager_Singleton) : nullptr;
    int32_t LastRegister = -1;
    for (int32_t Index = 0; VTable && AddConsoleObject && Index < 0x20; ++Index)
    {
        const uintptr_t Function = reinterpret_cast<uintptr_t>(VTable[Index]);
        const uintptr_t End = Finder::FunctionEnd(Function);
        if (End > Function && CallWithin(Function, End - Function, AddConsoleObject))
        {
            LastRegister = Index;
        }
    }

    if (LastRegister < 0)
    {
        Log::Error(L"Finder: IConsoleManager::ForEachConsoleObjectThatStartsWith not found");
        return false;
    }
    Natives::IConsoleManager_FindConsoleVariableIndex = LastRegister + 2;
    Natives::IConsoleManager_ForEachConsoleObjectThatStartsWithIndex = LastRegister + 4;
    Log::Info(L"Finder: IConsoleManager::FindConsoleVariable is slot 0x{:X}", Natives::IConsoleManager_FindConsoleVariableIndex);
    Log::Info(L"Finder: IConsoleManager::ForEachConsoleObjectThatStartsWith is slot 0x{:X}", Natives::IConsoleManager_ForEachConsoleObjectThatStartsWithIndex);
    return true;
}

bool Finder::ResolvePropertyCPPType()
{
    const uintptr_t PureVirtual = FunctionOf(L"UProperty::GetCPPType");
    if (void** PropertyVTable = CdoVTable(L"/Script/CoreUObject.Property"))
    {
        Natives::UProperty_GetCPPTypeIndex = VTableIndex(PropertyVTable, reinterpret_cast<void*>(PureVirtual), 0x100);
    }

    if (Natives::UProperty_GetCPPTypeIndex < 0)
    {
        Log::Error(L"Finder: UProperty::GetCPPType index not found");
        return false;
    }
    Log::Info(L"Finder: UProperty::GetCPPType is slot 0x{:X}", Natives::UProperty_GetCPPTypeIndex);
    return true;
}

bool Finder::ResolveScriptStructCopy()
{
    const int32_t StructOffset = ObjectLayout::Properties().UStructProperty_Struct;
    const auto ReadsStruct = [](uintptr_t Begin, uintptr_t End, int32_t Displacement)
    {
        for (uintptr_t Load = FindInstruction(Begin, End, { 0x8B, -1, Displacement }); Load; Load = FindInstruction(Load + 1, End, { 0x8B, -1, Displacement }))
        {
            if ((Byte(Load + 1) & 0xC7) == 0x41)
            {
                return true;
            }
        }
        return false;
    };

    void** VTable = CdoVTable(L"/Script/CoreUObject.StructProperty");
    std::vector<int32_t> StructSlots;
    const int32_t ArrayDimOffset = ObjectLayout::Properties().UProperty_ArrayDim;
    for (int32_t Index = 0; VTable && Index < 0x100; ++Index)
    {
        const uintptr_t Thunk = reinterpret_cast<uintptr_t>(VTable[Index]);
        if (IsInText(Thunk) && FindInstruction(Thunk, Thunk + 0x14, { 0x48, 0x8B, 0x41, StructOffset, 0x44, 0x8B, 0x41, ArrayDimOffset, 0x48, 0x8B, 0xC8, 0x4C, 0x8B, 0x08, 0x49, 0xFF, 0xA1 }) == Thunk)
        {
            StructSlots.push_back(*reinterpret_cast<const int32_t*>(Thunk + 17) / static_cast<int32_t>(sizeof(void*)));
        }
    }
    if (StructSlots.size() == 2)
    {
        Natives::UStruct_InitializeStructIndex = std::min(StructSlots[0], StructSlots[1]);
        Natives::UStruct_DestroyStructIndex = std::max(StructSlots[0], StructSlots[1]);
    }

    for (int32_t Index = 0; VTable && Index < 0x100 && !Natives::UScriptStruct_CopyScriptStruct; ++Index)
    {
        const uintptr_t Thunk = reinterpret_cast<uintptr_t>(VTable[Index]);
        if (!IsInText(Thunk) || FindInstruction(Thunk, Thunk + 9, { 0x48, 0x8B, 0x49, StructOffset, 0xE9 }) != Thunk)
        {
            continue;
        }

        const uintptr_t Target = Finder::RelativeTarget(Thunk + 4, 1, 5);
        if (IsInText(Target) && ReadsStruct(Target, Target + 0x30, ObjectLayout::Current.UStruct_MinAlignment) && ReadsStruct(Target, Target + 0x30, ObjectLayout::Current.UStruct_PropertiesSize))
        {
            Natives::UScriptStruct_CopyScriptStruct = reinterpret_cast<decltype(Natives::UScriptStruct_CopyScriptStruct)>(Target);
        }
    }

    if (!Natives::UScriptStruct_CopyScriptStruct || Natives::UStruct_InitializeStructIndex < 0)
    {
        Log::Error(L"Finder: UScriptStruct::CopyScriptStruct / UStruct::InitializeStruct / DestroyStruct not found");
        return false;
    }
    Log::Info(L"Finder: UScriptStruct::CopyScriptStruct found, UStruct::InitializeStruct slot 0x{:X}, DestroyStruct slot 0x{:X}", Natives::UStruct_InitializeStructIndex, Natives::UStruct_DestroyStructIndex);
    return true;
}

bool Finder::ResolveScriptFrame()
{
    UFunction* IsValidClass = FindObject<UFunction>(nullptr, L"/Script/Engine.KismetSystemLibrary:IsValidClass");
    const uintptr_t Thunk = IsValidClass ? reinterpret_cast<uintptr_t>(IsValidClass->GetNativeFunc()) : 0;
    const uintptr_t ThunkEnd = IsInText(Thunk) ? Finder::FunctionEnd(Thunk) : 0;

    ptrdiff_t Code = 0;
    if (const uintptr_t Compare = ThunkEnd ? FindInstruction(Thunk, ThunkEnd, { 0x48, 0x83, 0x7A, -1, 0x00 }) : 0)
    {
        Code = Byte(Compare + 3);
    }

    ptrdiff_t Object = 0;
    uintptr_t Step = 0;
    if (const uintptr_t Load = ThunkEnd ? FindInstruction(Thunk, ThunkEnd, { 0x48, 0x8B, 0x52, -1 }) : 0)
    {
        Object = Byte(Load + 3);
        const uintptr_t Call = FindRawCall(Load + 4, Load + 0x14);
        Step = Call ? Finder::RelativeTarget(Call, 1, 5) : 0;
    }

    ptrdiff_t PropertyChain = 0;
    uintptr_t StepExplicitProperty = 0;
    if (const uintptr_t Load = ThunkEnd ? FindInstruction(Thunk, ThunkEnd, { 0x4C, 0x8B, 0x82, -1, -1, 0x00, 0x00 }) : 0)
    {
        PropertyChain = *reinterpret_cast<const int32_t*>(Load + 3);
        const uintptr_t Call = FindRawCall(Load + 7, Load + 0x24);
        StepExplicitProperty = Call ? Finder::RelativeTarget(Call, 1, 5) : 0;
    }

    if (!Code || !Object || !PropertyChain || !Step || !StepExplicitProperty || Step == StepExplicitProperty)
    {
        Log::Error(L"Finder: FFrame not resolved from KismetSystemLibrary::IsValidClass");
        return false;
    }

    Natives::FFrame_Code = Code;
    Natives::FFrame_Object = Object;
    Natives::FFrame_PropertyChainForCompiledIn = PropertyChain;
    Natives::FFrame_Step = reinterpret_cast<decltype(Natives::FFrame_Step)>(Step);
    Natives::FFrame_StepExplicitProperty = reinterpret_cast<decltype(Natives::FFrame_StepExplicitProperty)>(StepExplicitProperty);
    Log::Info(L"Finder: FFrame Object +0x{:X}, Code +0x{:X}, PropertyChainForCompiledIn +0x{:X}", Object, Code, PropertyChain);
    return true;
}

bool Finder::ResolveTextLocalizationManager()
{
    uintptr_t FindText = 0;
    const UFunction* Function = FindObject<UFunction>(nullptr, L"/Script/Engine.KismetTextLibrary:FindTextInLocalizationTable");
    const uintptr_t Thunk = Function ? reinterpret_cast<uintptr_t>(Function->GetNativeFunc()) : 0;
    const uintptr_t ThunkEnd = Finder::FunctionEnd(Thunk);
    for (uintptr_t Call = Thunk && ThunkEnd ? FindRawCall(Thunk, ThunkEnd) : 0; Call && !FindText; Call = FindRawCall(Call + 5, ThunkEnd))
    {
        const uintptr_t Target = Finder::RelativeTarget(Call, 1, 5);
        if (Byte(Target) == 0x45 && Byte(Target + 1) == 0x33 && Byte(Target + 2) == 0xC9 && Byte(Target + 3) == 0xE9)
        {
            FindText = Finder::RelativeTarget(Target + 3, 1, 5);
        }
    }

    const uintptr_t ManagerLoad = FindText ? FindInstruction(FindText, FindText + 0x40, { 0x48, 0x8B, 0x05 }) : 0;
    const uintptr_t DisplayCall = FindText ? FindInstruction(FindText, FindText + 0x100, { 0x4C, 0x8B, 0xCF, 0x48, 0x89, 0x5C, 0x24, 0x20, 0x4C, 0x8B, 0xC6, 0x48, 0x8D, 0x55, -1, 0x48, 0x8B, 0xC8, 0xE8 }) : 0;
    if (!ManagerLoad || !DisplayCall)
    {
        Log::Error(L"Finder: FTextLocalizationManager::FindDisplayString not found");
        return false;
    }

    Natives::GTextLocalizationManager = reinterpret_cast<FTextLocalizationManager**>(Finder::RelativeTarget(ManagerLoad, 3, 7));
    Natives::FTextLocalizationManager_FindDisplayString = reinterpret_cast<decltype(Natives::FTextLocalizationManager_FindDisplayString)>(Finder::RelativeTarget(DisplayCall + 18, 1, 5));
    return true;
}

bool Finder::ResolvePlayerControllerConsole()
{
    for (const auto& [Name, Index] : { std::pair<const wchar_t*, int32_t*>{ L"/Script/Engine.PlayerController:ConsoleKey", &Natives::APlayerController_ConsoleKeyIndex }, std::pair<const wchar_t*, int32_t*>{ L"/Script/Engine.PlayerController:SendToConsole", &Natives::APlayerController_SendToConsoleIndex } })
    {
        const UFunction* Function = FindObject<UFunction>(nullptr, Name);
        const uintptr_t Thunk = Function ? reinterpret_cast<uintptr_t>(Function->GetNativeFunc()) : 0;
        const uintptr_t ThunkEnd = Finder::FunctionEnd(Thunk);
        const uintptr_t Call = Thunk && ThunkEnd ? FindInstruction(Thunk, ThunkEnd, { 0xFF, 0x90 }) : 0;
        *Index = Call ? *reinterpret_cast<const int32_t*>(Call + 2) / static_cast<int32_t>(sizeof(void*)) : -1;
        if (*Index < 0)
        {
            Log::Error(L"Finder: the virtual behind {} was not found", Name);
            return false;
        }
    }

    Log::Info(L"Finder: APlayerController::ConsoleKey is slot 0x{:X}, SendToConsole 0x{:X}", Natives::APlayerController_ConsoleKeyIndex, Natives::APlayerController_SendToConsoleIndex);
    return true;
}

bool Finder::ResolvePlayerControllerCheats()
{
    if (Natives::APlayerController_AddCheatsIndex >= 0)
    {
        return true;
    }

    // execEnableCheats tail-jumps to the EnableCheats virtual, whose body is AddCheats(false): mov rax, [rcx]; xor edx, edx; jmp [rax+disp32].
    const UFunction* Function = FindObject<UFunction>(nullptr, L"/Script/Engine.PlayerController:EnableCheats");
    const uintptr_t Thunk = Function ? reinterpret_cast<uintptr_t>(Function->GetNativeFunc()) : 0;
    const uintptr_t Jump = IsInText(Thunk) ? FindInstruction(Thunk, Thunk + 0x30, { 0x48, 0xFF, 0xA0 }) : 0;
    const int32_t EnableCheats = Jump ? *reinterpret_cast<const int32_t*>(Jump + 3) / static_cast<int32_t>(sizeof(void*)) : -1;
    const uintptr_t Body = EnableCheats >= 0 ? VTableEntry(CdoVTable(L"/Script/Engine.PlayerController"), EnableCheats) : 0;
    const bool bAddCheats = Body && FindInstruction(Body, Body + 8, { 0x48, 0x8B, 0x01, 0x33, 0xD2, 0x48, 0xFF, 0xA0 }) == Body;
    if (!bAddCheats)
    {
        Log::Error(L"Finder: APlayerController::AddCheats not found (EnableCheats slot {})", EnableCheats);
        return false;
    }

    Natives::APlayerController_AddCheatsIndex = *reinterpret_cast<const int32_t*>(Body + 8) / static_cast<int32_t>(sizeof(void*));
    Log::Info(L"Finder: APlayerController::EnableCheats is slot 0x{:X}, AddCheats slot 0x{:X}", EnableCheats, Natives::APlayerController_AddCheatsIndex);
    return true;
}

bool Finder::ResolveReplicationLive()
{
    MissingCount = 0;

    void** ActorVTable = CdoVTable(L"/Script/Engine.Actor");
    void** PlayerControllerVTable = CdoVTable(L"/Script/Engine.PlayerController");
    void** NetworkManagerVTable = CdoVTable(L"/Script/Engine.GameNetworkManager");
    const int32_t Velocity = Natives::AActor_GetVelocityIndex;
    const int32_t NetRelevant = Natives::AActor_IsNetRelevantForIndex;

    int32_t NetPriority = -1;
    int32_t NetPriorities = 0;
    for (int32_t Index = Velocity; ActorVTable && Velocity >= 0 && Index < NetRelevant; ++Index)
    {
        const uintptr_t Function = VTableEntry(ActorVTable, Index);
        const uintptr_t End = Finder::FunctionEnd(Function);
        for (uintptr_t Call = End ? FindVirtualCall(Function, End) : 0; Call; Call = FindVirtualCall(Call + 1, End))
        {
            if (VirtualSlot(Call) == Index)
            {
                NetPriority = Index;
                ++NetPriorities;
                break;
            }
        }
    }
    RequireSlot(L"AActor::GetNetPriority", Natives::AActor_GetNetPriorityIndex, NetPriorities == 1 ? NetPriority : -1);

    int32_t NetDormancy = -1;
    int32_t NetDormancies = 0;
    for (int32_t Index = NetPriority + 1; NetPriorities == 1 && Index < NetPriority + 8; ++Index)
    {
        const uintptr_t Function = VTableEntry(ActorVTable, Index);
        const uintptr_t End = Finder::FunctionEnd(Function);
        for (uintptr_t Jump = End ? FindVirtualCall(Function, End) : 0; Jump; Jump = FindVirtualCall(Jump + 1, End))
        {
            if ((Byte(Jump + 1) & 0x38) == 0x20 && VirtualSlot(Jump) == Index + 1)
            {
                NetDormancy = Index + 1;
                ++NetDormancies;
                break;
            }
        }
    }
    RequireSlot(L"AActor::GetNetDormancy", Natives::AActor_GetNetDormancyIndex, NetDormancies == 1 ? NetDormancy : -1);

    const uintptr_t NetRelevantFunction = FunctionOf(L"Actor %s / %s has no root component in AActor::IsNetRelevantFor. (Make bAlwaysRelevant=true?)");
    const uintptr_t RelevancyOwner = NetRelevant >= 0 && NetRelevantFunction && VTableEntry(ActorVTable, NetRelevant) == NetRelevantFunction ? VTableEntry(ActorVTable, NetRelevant + 2) : 0;
    const bool bRelevancyOwner = RelevancyOwner && FindInstruction(RelevancyOwner, RelevancyOwner + 7, { 0x4C, 0x3B, 0xC1, 0x0F, 0x94, 0xC0, 0xC3 }) == RelevancyOwner;
    RequireSlot(L"AActor::IsRelevancyOwnerFor", Natives::AActor_IsRelevancyOwnerForIndex, bRelevancyOwner ? NetRelevant + 2 : -1);

    int32_t NetOwner = -1;
    int32_t NetOwners = 0;
    for (int32_t Index = NetRelevant + 3; bRelevancyOwner && Index < NetRelevant + 0x10; ++Index)
    {
        const uintptr_t Function = VTableEntry(ActorVTable, Index);
        if (Function && FindInstruction(Function, Function + 8, { 0x48, 0x8B, 0x81, -1, -1, 0x00, 0x00, 0xC3 }) == Function)
        {
            NetOwner = Index;
            ++NetOwners;
        }
    }
    RequireSlot(L"AActor::GetNetOwner", Natives::AActor_GetNetOwnerIndex, NetOwners == 1 ? NetOwner : -1);

    const uintptr_t FlushDormancy = reinterpret_cast<uintptr_t>(Natives::AActor_FlushNetDormancy);
    const uintptr_t ModeCall = FlushDormancy ? FindRawCall(FlushDormancy, FlushDormancy + 0x40) : 0;
    const uintptr_t ActorNetMode = ModeCall && Byte(ModeCall + 5) == 0x83 && Byte(ModeCall + 6) == 0xF8 && Byte(ModeCall + 7) == 0x03 ? Finder::RelativeTarget(ModeCall, 1, 5) : 0;
    int32_t ClientAdjustment = -1;
    int32_t ClientAdjustments = 0;
    for (int32_t Index = Natives::AController_GetViewTargetIndex; ActorNetMode && Index > 0 && VTableEntry(PlayerControllerVTable, Index + 1); ++Index)
    {
        const uintptr_t CameraMode = VTableEntry(PlayerControllerVTable, Index);
        bool bCameraMode = false;
        for (uintptr_t Call = CameraMode ? FindVirtualCall(CameraMode, CameraMode + 0x40) : 0; Call && !bCameraMode; Call = FindVirtualCall(Call + 1, CameraMode + 0x40))
        {
            bCameraMode = VirtualSlot(Call) == Index - 1;
        }

        const uintptr_t Adjustment = VTableEntry(PlayerControllerVTable, Index + 1);
        bool bAutonomousProxy = false;
        for (uintptr_t Test = bCameraMode ? FindInstruction(Adjustment, Adjustment + 0x100, { 0x80, -1, -1, 0x00, 0x00, 0x00, 0x02 }) : 0; Test && !bAutonomousProxy; Test = FindInstruction(Test + 1, Adjustment + 0x100, { 0x80, -1, -1, 0x00, 0x00, 0x00, 0x02 }))
        {
            bAutonomousProxy = (Byte(Test + 1) & 0xF8) == 0xB8 && (Byte(Test + 1) & 0x07) != 0x04;
        }
        bool bNotClient = false;
        for (uintptr_t Call = bAutonomousProxy ? FindRawCall(Adjustment, Adjustment + 0x100) : 0; Call && !bNotClient; Call = FindRawCall(Call + 1, Adjustment + 0x100))
        {
            bNotClient = Finder::RelativeTarget(Call, 1, 5) == ActorNetMode && Byte(Call + 5) == 0x83 && Byte(Call + 6) == 0xF8 && Byte(Call + 7) == 0x03;
        }
        if (bNotClient)
        {
            ClientAdjustment = Index + 1;
            ++ClientAdjustments;
        }
    }
    RequireSlot(L"APlayerController::SendClientAdjustment", Natives::APlayerController_SendClientAdjustmentIndex, ClientAdjustments == 1 ? ClientAdjustment : -1);

    const int32_t Standby = Finder::VTableIndex(NetworkManagerVTable, reinterpret_cast<void*>(FunctionOf(L"Standby check is %s with RxTime (%f), TxTime (%f), PingThreshold (%d), JoinInProgressStandbyWaitTime (%f)")), 0x400);
    RequireSlot(L"AGameNetworkManager::IsInLowBandwidthMode", Natives::AGameNetworkManager_IsInLowBandwidthModeIndex, Standby >= 5 && VTableEntry(NetworkManagerVTable, Standby - 5) ? Standby - 5 : -1);

    const uintptr_t ReplicateActor = reinterpret_cast<uintptr_t>(Natives::UActorChannel_ReplicateActor);
    const uintptr_t ReplicateActorEnd = Finder::FunctionEnd(ReplicateActor);
    const uintptr_t WorldSettingsCall = ReplicateActorEnd ? FindInstruction(ReplicateActor, ReplicateActorEnd, { 0x41, 0xB0, 0x01, 0x33, 0xD2, 0x48, 0x8B, 0xC8, 0xE8 }) : 0;
    const uintptr_t ViewersNum = WorldSettingsCall ? FindInstruction(WorldSettingsCall + 13, WorldSettingsCall + 0x20, { 0x39, -1 }) : 0;
    const uintptr_t ViewersStride = ViewersNum ? FindInstruction(ViewersNum, ViewersNum + 0x30, { 0x48, 0x8D, -1, 0x52, 0x48, 0xC1, -1, 0x04 }) : 0;
    const uintptr_t ViewersData = ViewersStride ? FindInstruction(ViewersStride, ViewersStride + 0x10, { 0x49, 0x03, -1, 0x24 }) : 0;
    const UProperty* ViewersProperty = AWorldSettings::StaticClass() ? AWorldSettings::StaticClass()->FindPropertyByName(FName(L"ReplicationViewers", FNAME_Find)) : nullptr;
    const UScriptStruct* NetViewerStruct = FindObject<UScriptStruct>(nullptr, L"/Script/Engine.NetViewer");
    const int32_t ReflectedViewers = ViewersProperty ? ViewersProperty->GetOffset_ForInternal() : -1;
    const bool bViewersLayout = ViewersData && (Byte(ViewersNum + 1) & 0xC0) == 0x80
        && *reinterpret_cast<const int32_t*>(ViewersData + 4) == ReflectedViewers
        && *reinterpret_cast<const int32_t*>(ViewersNum + 2) == ReflectedViewers + 8
        && NetViewerStruct && NetViewerStruct->GetPropertiesSize() == static_cast<int32_t>(sizeof(FNetViewer));
    if (!bViewersLayout)
    {
        Log::Error(L"Finder: AWorldSettings::ReplicationViewers layout differs from ReplicateActor (reflected 0x{:X}, FNetViewer {} bytes)", ReflectedViewers, NetViewerStruct ? NetViewerStruct->GetPropertiesSize() : -1);
        ++MissingCount;
    }

    Log::Info(L"Finder: replication live slots resolved, {} missing", MissingCount);
    return MissingCount == 0;
}

int32_t Finder::ImplementationIndex(UFunction* Function)
{
    const uintptr_t Exec = Function ? reinterpret_cast<uintptr_t>(Function->GetNativeFunc()) : 0;
    const uintptr_t ExecEnd = IsInText(Exec) ? Finder::FunctionEnd(Exec) : 0;
    int32_t Index = -1;
    for (uintptr_t Site = ExecEnd ? FindVirtualCall(Exec, ExecEnd) : 0; Site; Site = FindVirtualCall(Site + 1, ExecEnd))
    {
        Index = VirtualSlot(Site);
    }
    return Index;
}

bool Finder::ResolveInventoryOwnerLive(UFunction* ServerRemoveInventoryItem, UObject* PlayerControllerDefault, ptrdiff_t WorldInventoryOffset)
{
    void** PlayerControllerVTable = PlayerControllerDefault ? *reinterpret_cast<void***>(PlayerControllerDefault) : nullptr;
    const uintptr_t Implementation = VTableEntry(PlayerControllerVTable, ImplementationIndex(ServerRemoveInventoryItem));
    const uintptr_t InterfaceAdjust = Implementation ? FindInstruction(Implementation, Implementation + 0x20, { 0x48, 0x81, 0xC1 }) : 0;
    const int32_t RemoveIndex = InterfaceAdjust ? VirtualSlot(FindVirtualCall(InterfaceAdjust, InterfaceAdjust + 0x20)) : -1;
    if (!InterfaceAdjust || RemoveIndex < 0)
    {
        Log::Error(L"Finder: IFortInventoryOwnerInterface::RemoveInventoryItem not found");
        return false;
    }

    const int32_t InterfaceOffset = *reinterpret_cast<const int32_t*>(InterfaceAdjust + 3);
    void** InterfaceVTable = *reinterpret_cast<void***>(reinterpret_cast<uint8_t*>(PlayerControllerDefault) + InterfaceOffset);
    int32_t GetWorldInventoryIndex = -1;
    for (int32_t Index = 0; Index < RemoveIndex && GetWorldInventoryIndex < 0; ++Index)
    {
        const uintptr_t Function = VTableEntry(InterfaceVTable, Index);
        if (Function && Byte(Function) == 0x48 && Byte(Function + 1) == 0x8B && Byte(Function + 2) == 0x81 && Byte(Function + 7) == 0xC3
            && InterfaceOffset + *reinterpret_cast<const int32_t*>(Function + 3) == WorldInventoryOffset)
        {
            GetWorldInventoryIndex = Index;
        }
    }
    if (GetWorldInventoryIndex < 0)
    {
        Log::Error(L"Finder: IFortInventoryOwnerInterface::GetWorldInventory not found");
        return false;
    }

    Natives::AFortPlayerController_InventoryOwnerInterface = InterfaceOffset;
    Natives::IFortInventoryOwnerInterface_RemoveInventoryItemIndex = RemoveIndex;
    Natives::IFortInventoryOwnerInterface_GetWorldInventoryIndex = GetWorldInventoryIndex;
    Log::Info(L"Finder: IFortInventoryOwnerInterface is AFortPlayerController +0x{:X}, GetWorldInventory slot 0x{:X}, RemoveInventoryItem slot 0x{:X}", InterfaceOffset, GetWorldInventoryIndex, RemoveIndex);
    return true;
}

bool Finder::ResolvePickupLive()
{
    const auto LastCall = [](const wchar_t* FunctionPath)
    {
        UFunction* Function = FindObject<UFunction>(nullptr, FunctionPath);
        const uintptr_t Exec = Function ? reinterpret_cast<uintptr_t>(Function->GetNativeFunc()) : 0;
        const uintptr_t ExecEnd = IsInText(Exec) ? Finder::FunctionEnd(Exec) : 0;
        uintptr_t Native = 0;
        for (uintptr_t Call = ExecEnd ? FindRawCall(Exec, ExecEnd) : 0; Call; Call = FindRawCall(Call + 1, ExecEnd))
        {
            Native = Finder::RelativeTarget(Call, 1, 5);
        }
        return Native;
    };

    // SpawnMissionItemPickup builds FFortItemEntry(Entry, ItemDefinition, Count, Level, nullptr) and passes it straight on
    // to CreatePickup, in every build (1.7.2 inlines the constructor into CreateTemporaryItemInstance, so that function no
    // longer leads to it).
    const uintptr_t SpawnMissionItemPickup = LastCall(L"/Script/FortniteGame.FortMissionLibrary:SpawnMissionItemPickup");
    const uintptr_t SpawnMissionItemPickupEnd = SpawnMissionItemPickup ? Finder::FunctionEnd(SpawnMissionItemPickup) : 0;
    const uintptr_t EntrySetup = SpawnMissionItemPickupEnd ? FindInstruction(SpawnMissionItemPickup, SpawnMissionItemPickupEnd,
        { 0x48, 0xC7, 0x44, 0x24, 0x20, 0x00, 0x00, 0x00, 0x00, 0x45, 0x8D, 0x41, 0x01, 0xE8, -1, -1, -1, -1, 0x48, 0x8B, 0xD0 }) : 0;
    const uintptr_t EntryConstructor = EntrySetup ? Finder::RelativeTarget(EntrySetup + 13, 1, 5) : 0;
    const uintptr_t Next = EntrySetup ? FindRawCall(EntrySetup + 18, SpawnMissionItemPickupEnd) : 0;
    const uintptr_t CreatePickup = Next ? Finder::RelativeTarget(Next, 1, 5) : 0;

    if (!EntryConstructor || !CreatePickup)
    {
        Log::Error(L"Finder: FFortItemEntry::FFortItemEntry / AFortPickup::CreatePickup not found");
        return false;
    }
    Natives::FFortItemEntry_FFortItemEntry = reinterpret_cast<decltype(Natives::FFortItemEntry_FFortItemEntry)>(EntryConstructor);
    Natives::AFortPickup_CreatePickup = reinterpret_cast<decltype(Natives::AFortPickup_CreatePickup)>(CreatePickup);
    Log::Info(L"Finder: FFortItemEntry::FFortItemEntry and AFortPickup::CreatePickup found");
    return true;
}

bool Finder::ResolvePickupGiveLive()
{
    const auto LastCall = [](uintptr_t Function)
    {
        const uintptr_t FunctionEnd = IsInText(Function) ? Finder::FunctionEnd(Function) : 0;
        uintptr_t Target = 0;
        for (uintptr_t Call = FunctionEnd ? FindRawCall(Function, FunctionEnd) : 0; Call; Call = FindRawCall(Call + 1, FunctionEnd))
        {
            Target = Finder::RelativeTarget(Call, 1, 5);
        }
        return Target;
    };

    UFunction* SpawnMissionItemPickupFunction = FindObject<UFunction>(nullptr, L"/Script/FortniteGame.FortMissionLibrary:SpawnMissionItemPickup");
    const uintptr_t SpawnMissionItemPickup = SpawnMissionItemPickupFunction ? LastCall(reinterpret_cast<uintptr_t>(SpawnMissionItemPickupFunction->GetNativeFunc())) : 0;
    const uintptr_t SetPickupTarget = SpawnMissionItemPickup ? LastCall(SpawnMissionItemPickup) : 0;
    const uintptr_t SetPickupTargetEnd = SetPickupTarget ? Finder::FunctionEnd(SetPickupTarget) : 0;
    void** PickupVTable = CdoVTable(L"/Script/FortniteGame.FortPickup");

    int32_t GiveIndex = -1;
    for (uintptr_t Call = SetPickupTargetEnd && PickupVTable ? FindRawCall(SetPickupTarget, SetPickupTargetEnd) : 0; Call && GiveIndex < 0; Call = FindRawCall(Call + 1, SetPickupTargetEnd))
    {
        const uintptr_t Callee = Finder::RelativeTarget(Call, 1, 5);
        const uintptr_t CalleeEnd = Finder::FunctionEnd(Callee);
        for (uintptr_t Lea = CalleeEnd ? FindInstruction(Callee, CalleeEnd, { 0x48, 0x8D, 0x05 }) : 0; Lea && GiveIndex < 0; Lea = FindInstruction(Lea + 1, CalleeEnd, { 0x48, 0x8D, 0x05 }))
        {
            const uintptr_t Delegate = Finder::RelativeTarget(Lea, 3, 7);
            if (!IsInText(Delegate) || Delegate == reinterpret_cast<uintptr_t>(Natives::SharedEmptyFunction) || Finder::FunctionStart(Delegate) != Delegate)
            {
                continue;
            }

            const uintptr_t DelegateEnd = Finder::FunctionEnd(Delegate);
            uintptr_t LastVirtualCall = 0;
            int32_t VirtualCalls = 0;
            for (uintptr_t Site = DelegateEnd ? FindVirtualCall(Delegate, DelegateEnd) : 0; Site; Site = FindVirtualCall(Site + 1, DelegateEnd))
            {
                LastVirtualCall = Site;
                ++VirtualCalls;
            }

            const int32_t Slot = VirtualCalls >= 2 ? VirtualSlot(LastVirtualCall) : -1;
            if (Slot >= 0 && reinterpret_cast<void*>(VTableEntry(PickupVTable, Slot)) == Natives::SharedEmptyFunction)
            {
                GiveIndex = Slot;
            }
        }
    }

    if (GiveIndex < 0)
    {
        Log::Error(L"Finder: AFortPickup::GivePickupTo slot not found");
        return false;
    }
    Natives::AFortPickup_GivePickupToIndex = GiveIndex;
    Natives::AFortPickup_SetPickupTarget = reinterpret_cast<decltype(Natives::AFortPickup_SetPickupTarget)>(SetPickupTarget);
    Log::Info(L"Finder: AFortPickup::GivePickupTo is slot 0x{:X}, SetPickupTarget +0x{:X}", GiveIndex, SetPickupTarget - Finder::ImageBase());
    return true;
}

bool Finder::ResolvePlayerDropPickupLive()
{
    if (!Natives::AFortPickup_CreatePickup && !ResolvePickupLive())
    {
        return false;
    }

    const uintptr_t CreatePickup = reinterpret_cast<uintptr_t>(Natives::AFortPickup_CreatePickup);
    void** VTable = CdoVTable(L"/Script/FortniteGame.FortPlayerController");
    int32_t DropPickupIndex = -1;
    for (int32_t Index = 0; VTable && Index < 0x400 && DropPickupIndex < 0; ++Index)
    {
        const uintptr_t Function = VTableEntry(VTable, Index);
        const uintptr_t FunctionEnd = Function ? Finder::FunctionEnd(Function) : 0;
        if (FunctionEnd > Function && FunctionEnd - Function < 0x100 && CallWithin(Function, FunctionEnd - Function, CreatePickup))
        {
            DropPickupIndex = Index;
        }
    }

    if (DropPickupIndex < 0)
    {
        Log::Error(L"Finder: AFortPlayerController::SpawnDropPickup slot not found");
        return false;
    }
    Natives::AFortPlayerController_SpawnDropPickupIndex = DropPickupIndex;
    Log::Info(L"Finder: AFortPlayerController::SpawnDropPickup is slot 0x{:X}", DropPickupIndex);
    return true;
}

bool Finder::ResolveMissionGenerationLive()
{
    UFunction* LoadMissionFunction = FindObject<UFunction>(nullptr, L"/Script/FortniteGame.FortMissionLibrary:LoadMission");
    const uintptr_t Exec = LoadMissionFunction ? reinterpret_cast<uintptr_t>(LoadMissionFunction->GetNativeFunc()) : 0;
    const uintptr_t ExecEnd = IsInText(Exec) ? Finder::FunctionEnd(Exec) : 0;
    uintptr_t LoadMission = 0;
    for (uintptr_t Call = ExecEnd ? FindRawCall(Exec, ExecEnd) : 0; Call; Call = FindRawCall(Call + 1, ExecEnd))
    {
        LoadMission = Finder::RelativeTarget(Call, 1, 5);
    }

    const uintptr_t Generate = FunctionOf(L"%s did not spend enough difficulty option points to reach the minimum cost bar!");
    const uintptr_t LoadMissionEnd = LoadMission ? Finder::FunctionEnd(LoadMission) : 0;
    uintptr_t GenerateCall = 0;
    for (uintptr_t Call = LoadMissionEnd && Generate ? FindRawCall(LoadMission, LoadMissionEnd) : 0; Call && !GenerateCall; Call = FindRawCall(Call + 1, LoadMissionEnd))
    {
        if (Finder::RelativeTarget(Call, 1, 5) == Generate)
        {
            GenerateCall = Call;
        }
    }

    ptrdiff_t StreamOffset = -1;
    for (uintptr_t Site = GenerateCall ? GenerateCall - 0x30 : 0; Site && Site + 7 <= GenerateCall; ++Site)
    {
        if ((Byte(Site) == 0x4C || Byte(Site) == 0x4D) && Byte(Site + 1) == 0x8D && (Byte(Site + 2) & 0xF8) == 0x80 && (Byte(Site + 2) & 0x07) != 0x04)
        {
            StreamOffset = *reinterpret_cast<const int32_t*>(Site + 3);
        }
    }

    const uintptr_t TableLea = LoadMission && GenerateCall ? FindInstruction(LoadMission, GenerateCall, { 0x48, 0x8D, 0x88 }) : 0;

    if (!Generate || StreamOffset <= 0 || !TableLea)
    {
        Log::Error(L"Finder: UFortMissionInfo::GenerateMissionDifficultyOptions inputs not found");
        return false;
    }
    Natives::UFortMissionInfo_GenerateMissionDifficultyOptions = reinterpret_cast<decltype(Natives::UFortMissionInfo_GenerateMissionDifficultyOptions)>(Generate);
    Natives::AFortWorldManager_MissionGenerationRandomStream = StreamOffset;
    Natives::UFortGameData_MissionGenerationData = *reinterpret_cast<const int32_t*>(TableLea + 3);
    Log::Info(L"Finder: UFortMissionInfo::GenerateMissionDifficultyOptions found, world manager stream +0x{:X}, game data table +0x{:X}", StreamOffset, Natives::UFortGameData_MissionGenerationData);
    return true;
}

bool Finder::ResolveZoneEndLive(ptrdiff_t MissionInfoOffset, ptrdiff_t ZoneFinishedOffset, ptrdiff_t ZoneCompletedOffset, ptrdiff_t EndOfZoneRemainTimeOffset, ptrdiff_t ActiveSpawnPadOffset)
{
    if (Natives::AFortMissionManager_OnZoneEndIndex >= 0 && Natives::AFortMissionManager_OnMissionStatusChangeIndex >= 0 && Natives::AFortGameModeZone_OnZoneEndIndex >= 0 && Natives::AFortGameStateZone_SetGameplayStateIndex >= 0 && Natives::UFortQuestManager_SendStatEvent && Natives::UFortQuestManager_SendEndPrimaryMission && Natives::UGameInstance_GetGameSessionId && Natives::UEnum_GetEnumName)
    {
        return true;
    }

    const auto ExecOf = [](const wchar_t* FunctionPath)
    {
        UFunction* Function = FindObject<UFunction>(nullptr, FunctionPath);
        return Function ? reinterpret_cast<uintptr_t>(Function->GetNativeFunc()) : 0;
    };
    const auto Calls = [](uintptr_t Function)
    {
        std::vector<std::pair<uintptr_t, uintptr_t>> Result;
        const uintptr_t End = IsInText(Function) ? Finder::FunctionEnd(Function) : 0;
        for (uintptr_t Call = End ? FindRawCall(Function, End) : 0; Call; Call = FindRawCall(Call + 1, End))
        {
            Result.emplace_back(Call, Finder::RelativeTarget(Call, 1, 5));
        }
        return Result;
    };
    const auto Bytes32 = [](ptrdiff_t Value)
    {
        const int32_t V = static_cast<int32_t>(Value);
        return std::vector<int32_t>{ V & 0xFF, (V >> 8) & 0xFF, (V >> 16) & 0xFF, (V >> 24) & 0xFF };
    };
    const auto Contains = [](uintptr_t Begin, uintptr_t End, const std::vector<int32_t>& Pattern)
    {
        for (uintptr_t Address = Begin; Address + Pattern.size() <= End; ++Address)
        {
            size_t Index = 0;
            while (Index < Pattern.size() && (Pattern[Index] < 0 || Byte(Address + Index) == static_cast<uint8_t>(Pattern[Index])))
            {
                ++Index;
            }
            if (Index == Pattern.size())
            {
                return Address;
            }
        }
        return uintptr_t{ 0 };
    };

    const auto Step = reinterpret_cast<uintptr_t>(Natives::FFrame_Step);
    const auto StepExplicit = reinterpret_cast<uintptr_t>(Natives::FFrame_StepExplicitProperty);
    uintptr_t SourceAndContextTags = 0;
    for (const auto& [Call, Target] : Calls(ExecOf(L"/Script/FortniteGame.FortQuestManager:GetSourceAndContextTags")))
    {
        if (!SourceAndContextTags && Target != Step && Target != StepExplicit)
        {
            SourceAndContextTags = Target;
        }
    }

    const auto WaveCalls = Calls(ExecOf(L"/Script/FortniteGame.FortMission:OnWaveCompleted"));
    const uintptr_t WaveCompleted = WaveCalls.empty() ? 0 : WaveCalls.back().second;
    uintptr_t SendStatEvent = 0;
    bool bAfterTags = false;
    for (const auto& [Call, Target] : WaveCompleted && SourceAndContextTags ? Calls(WaveCompleted) : decltype(Calls(0)){})
    {
        if (bAfterTags && !SendStatEvent)
        {
            SendStatEvent = Target;
        }
        bAfterTags |= Target == SourceAndContextTags;
    }

    const auto RemoveCalls = Calls(ExecOf(L"/Script/FortniteGame.FortMission:RemoveParticipantAccount"));
    const uintptr_t RemoveParticipant = RemoveCalls.empty() ? 0 : RemoveCalls.back().second;
    const uintptr_t RemoveEnd = RemoveParticipant ? Finder::FunctionEnd(RemoveParticipant) : 0;
    const uintptr_t FirstVirtual = RemoveEnd ? FindVirtualCall(RemoveParticipant, RemoveEnd) : 0;

    std::vector<uintptr_t> DedicatedServerCommands;
    for (uintptr_t Reference : Finder::StringRefs(L"McpDedicatedServerCommandUrl"))
    {
        DedicatedServerCommands.push_back(Finder::FunctionStart(Reference));
    }

    const auto FindObjectNative = reinterpret_cast<uintptr_t>(Natives::StaticFindObject);
    uintptr_t GameSessionId = 0;
    uintptr_t SendEndPrimaryMission = 0;
    uintptr_t GetEnumName = 0;
    uintptr_t FindObjectCall = 0;
    uintptr_t PreviousTarget = 0;
    const auto RemoveParticipantCalls = RemoveParticipant ? Calls(RemoveParticipant) : decltype(Calls(0)){};
    for (const auto& [Call, Target] : RemoveParticipantCalls)
    {
        if (FirstVirtual && Call < FirstVirtual)
        {
            GameSessionId = Target;
        }
        if (!SendEndPrimaryMission && std::find(DedicatedServerCommands.begin(), DedicatedServerCommands.end(), Target) != DedicatedServerCommands.end())
        {
            SendEndPrimaryMission = Target;
        }
        if (!FindObjectCall && Target == FindObjectNative)
        {
            FindObjectCall = Call;
        }
    }
    std::vector<int32_t> InfoLoad{ 0x48, 0x8B, -1 };
    for (int32_t Value : Bytes32(MissionInfoOffset))
    {
        InfoLoad.push_back(Value);
    }
    const uintptr_t Load = FindObjectCall ? Contains(FindObjectCall, RemoveEnd, InfoLoad) : 0;
    for (const auto& [Call, Target] : RemoveParticipantCalls)
    {
        if (Load && Call > FindObjectCall && Call < Load)
        {
            GetEnumName = Target;
        }
    }

    int32_t MissionManagerIndex = -1;
    uintptr_t ZoneEndTimer = 0;
    const auto Finished = Bytes32(ZoneFinishedOffset);
    const auto Completed = Bytes32(ZoneCompletedOffset);
    for (const auto& [First, Second] : { std::pair{ Finished, Completed }, std::pair{ Completed, Finished } })
    {
        std::vector<int32_t> Pattern{ 0xC6, 0x81 };
        Pattern.insert(Pattern.end(), First.begin(), First.end());
        Pattern.insert(Pattern.end(), { 0x01, 0x88, 0x91 });
        Pattern.insert(Pattern.end(), Second.begin(), Second.end());
        Pattern.push_back(0xC3);
        const uintptr_t Setter = Contains(Finder::TextBegin(), Finder::TextEnd(), Pattern);
        if (!Setter || MissionManagerIndex >= 0)
        {
            continue;
        }

        for (uintptr_t Site = Finder::TextBegin(); Site + 5 <= Finder::TextEnd() && MissionManagerIndex < 0; ++Site)
        {
            if (Byte(Site) != 0xE8 || Finder::RelativeTarget(Site, 1, 5) != Setter)
            {
                continue;
            }
            const uintptr_t Timer = Finder::FunctionStart(Site);
            const uintptr_t TimerEnd = Timer ? Finder::FunctionEnd(Timer) : 0;
            const uintptr_t Tail = TimerEnd ? FindVirtualCall(Site + 5, TimerEnd) : 0;
            MissionManagerIndex = Tail ? VirtualSlot(Tail) : -1;
            ZoneEndTimer = MissionManagerIndex >= 0 ? Timer : 0;
        }
    }

    int32_t StatusChangeIndex = -1;
    void** MissionManagerVTable = CdoVTable(L"/Script/FortniteGame.FortMissionManager");
    for (uintptr_t Site = ZoneEndTimer ? Finder::TextBegin() : 0; Site && Site + 7 <= Finder::TextEnd() && StatusChangeIndex < 0; ++Site)
    {
        if (Byte(Site) == 0x48 && Byte(Site + 1) == 0x8D && (Byte(Site + 2) & 0xC7) == 0x05 && Finder::RelativeTarget(Site, 3, 7) == ZoneEndTimer)
        {
            StatusChangeIndex = Finder::VTableIndex(MissionManagerVTable, reinterpret_cast<void*>(Finder::FunctionStart(Site)), 0x400);
        }
    }

    const auto FirstEntryContaining = [](void** VTable, const std::vector<std::vector<int32_t>>& Patterns, const auto& ContainsFn)
    {
        for (int32_t Index = 0; VTable && Index < 0x400; ++Index)
        {
            const uintptr_t Function = VTableEntry(VTable, Index);
            if (!Function)
            {
                continue;
            }
            const uintptr_t FunctionEnd = Finder::FunctionEnd(Function);
            bool bAll = FunctionEnd > Function && FunctionEnd - Function < 0x2000;
            for (const auto& Pattern : Patterns)
            {
                bAll = bAll && ContainsFn(Function, FunctionEnd, Pattern);
            }
            if (bAll)
            {
                return Index;
            }
        }
        return -1;
    };

    const int32_t GameModeIndex = FirstEntryContaining(CdoVTable(L"/Script/FortniteGame.FortGameModeZone"), { Bytes32(EndOfZoneRemainTimeOffset), Bytes32(ActiveSpawnPadOffset) }, Contains);

    void** GameStateVTable = CdoVTable(L"/Script/FortniteGame.FortGameStateZone");
    const uintptr_t OnRepGameplayState = FunctionOf(L"Zone has been completed, OnZoneCompleteEvent event triggering HUD EndOfZone");
    int32_t OnRepIndex = -1;
    for (int32_t Index = 0; GameStateVTable && OnRepGameplayState && Index < 0x400 && OnRepIndex < 0; ++Index)
    {
        const uintptr_t Entry = VTableEntry(GameStateVTable, Index);
        OnRepIndex = Entry == OnRepGameplayState ? Index : -1;
    }
    std::vector<int32_t> OnRepCall{ 0xFF, 0x90 };
    for (int32_t Value : Bytes32(OnRepIndex * 8))
    {
        OnRepCall.push_back(Value);
    }
    // AFortGameState::SetGameplayState sets the state and calls OnRep_GameplayState. 1.1's and 1.2's zone override inlines
    // it; 1.7.2's (0x5C6400) calls it as Super, so the base class's vtable is searched then (same slot in the zone class).
    int32_t GameStateIndex = OnRepIndex >= 0 ? FirstEntryContaining(GameStateVTable, { OnRepCall }, Contains) : -1;
    if (OnRepIndex >= 0 && GameStateIndex < 0)
    {
        GameStateIndex = FirstEntryContaining(CdoVTable(L"/Script/FortniteGame.FortGameState"), { OnRepCall }, Contains);
    }

    if (!SendStatEvent || !SendEndPrimaryMission || !GameSessionId || !GetEnumName || MissionManagerIndex < 0 || StatusChangeIndex < 0 || GameModeIndex < 0 || GameStateIndex < 0)
    {
        Log::Error(L"Finder: zone end not found (stat {}, end mission {}, session {}, enum {}, mission manager {}, status change {}, game mode {}, game state {})", SendStatEvent != 0, SendEndPrimaryMission != 0, GameSessionId != 0, GetEnumName != 0, MissionManagerIndex, StatusChangeIndex, GameModeIndex, GameStateIndex);
        return false;
    }

    Natives::UFortQuestManager_SendStatEvent = reinterpret_cast<decltype(Natives::UFortQuestManager_SendStatEvent)>(SendStatEvent);
    Natives::UFortQuestManager_SendEndPrimaryMission = reinterpret_cast<decltype(Natives::UFortQuestManager_SendEndPrimaryMission)>(SendEndPrimaryMission);
    Natives::UGameInstance_GetGameSessionId = reinterpret_cast<decltype(Natives::UGameInstance_GetGameSessionId)>(GameSessionId);
    Natives::UEnum_GetEnumName = reinterpret_cast<decltype(Natives::UEnum_GetEnumName)>(GetEnumName);
    Natives::AFortMissionManager_OnZoneEndIndex = MissionManagerIndex;
    Natives::AFortMissionManager_OnMissionStatusChangeIndex = StatusChangeIndex;
    Natives::AFortGameModeZone_OnZoneEndIndex = GameModeIndex;
    Natives::AFortGameStateZone_SetGameplayStateIndex = GameStateIndex;
    Log::Info(L"Finder: zone end resolved: AFortMissionManager::OnZoneEnd slot 0x{:X}, OnMissionStatusChange slot 0x{:X}, AFortGameModeZone::OnZoneEnd slot 0x{:X}, AFortGameStateZone::SetGameplayState slot 0x{:X}", MissionManagerIndex, StatusChangeIndex, GameModeIndex, GameStateIndex);
    return true;
}

namespace
{
    uintptr_t ExecImplementation(const wchar_t* FunctionPath)
    {
        UFunction* Function = FindObject<UFunction>(nullptr, FunctionPath);
        const uintptr_t Exec = Function ? reinterpret_cast<uintptr_t>(Function->GetNativeFunc()) : 0;
        const uintptr_t ExecEnd = IsInText(Exec) ? Finder::FunctionEnd(Exec) : 0;
        const auto Step = reinterpret_cast<uintptr_t>(Natives::FFrame_Step);
        const auto StepExplicit = reinterpret_cast<uintptr_t>(Natives::FFrame_StepExplicitProperty);
        uintptr_t Implementation = 0;
        bool bAfterStep = false;
        for (uintptr_t Call = ExecEnd ? FindRawCall(Exec, ExecEnd) : 0; Call; Call = FindRawCall(Call + 1, ExecEnd))
        {
            const uintptr_t Target = Finder::RelativeTarget(Call, 1, 5);
            if (Target == Step || Target == StepExplicit)
            {
                bAfterStep = true;
                Implementation = 0;
            }
            else if (bAfterStep && !Implementation)
            {
                Implementation = Target;
            }
        }
        return Implementation;
    }
}

uint8_t* Finder::ImplementationCall(UFunction* Function)
{
    const uintptr_t Exec = Function ? reinterpret_cast<uintptr_t>(Function->GetNativeFunc()) : 0;
    const uintptr_t ExecEnd = IsInText(Exec) ? Finder::FunctionEnd(Exec) : 0;
    const auto Step = reinterpret_cast<uintptr_t>(Natives::FFrame_Step);
    const auto StepExplicit = reinterpret_cast<uintptr_t>(Natives::FFrame_StepExplicitProperty);
    uintptr_t Implementation = 0;
    bool bAfterStep = false;
    for (uintptr_t Call = ExecEnd ? FindRawCall(Exec, ExecEnd) : 0; Call; Call = FindRawCall(Call + 1, ExecEnd))
    {
        const uintptr_t Target = Finder::RelativeTarget(Call, 1, 5);
        if (Target == Step || Target == StepExplicit)
        {
            bAfterStep = true;
            Implementation = 0;
        }
        else if (bAfterStep && !Implementation)
        {
            Implementation = Call;
        }
    }
    return reinterpret_cast<uint8_t*>(Implementation);
}

uint8_t* Finder::FirstCallInImplementation(UFunction* Function, uintptr_t Target)
{
    const uintptr_t Exec = Function ? reinterpret_cast<uintptr_t>(Function->GetNativeFunc()) : 0;
    const uintptr_t ExecEnd = IsInText(Exec) ? Finder::FunctionEnd(Exec) : 0;
    for (uintptr_t Call = ExecEnd ? FindRawCall(Exec, ExecEnd) : 0; Call; Call = FindRawCall(Call + 1, ExecEnd))
    {
        const uintptr_t Implementation = Finder::RelativeTarget(Call, 1, 5);
        const uintptr_t ImplementationEnd = Finder::FunctionStart(Implementation) == Implementation ? Finder::FunctionEnd(Implementation) : 0;
        const uintptr_t First = ImplementationEnd ? FindRawCall(Implementation, ImplementationEnd) : 0;
        if (First && Finder::RelativeTarget(First, 1, 5) == Target)
        {
            return reinterpret_cast<uint8_t*>(First);
        }
    }
    return nullptr;
}

bool Finder::ResolveGoalManagerLive()
{
    const uintptr_t AddGoal = ExecImplementation(L"/Script/FortniteGame.FortAIGoalManager:AddGoalToWorldAssignment");
    const uintptr_t AddGoalEnd = AddGoal ? Finder::FunctionEnd(AddGoal) : 0;
    uintptr_t FindInList = 0;
    uintptr_t AssignmentAddGoal = 0;
    for (uintptr_t Call = AddGoalEnd ? FindRawCall(AddGoal, AddGoalEnd) : 0; Call; Call = FindRawCall(Call + 1, AddGoalEnd))
    {
        FindInList = AssignmentAddGoal;
        AssignmentAddGoal = Finder::RelativeTarget(Call, 1, 5);
    }

    int32_t GoalsOffset = -1;
    const uintptr_t AssignmentAddGoalEnd = AssignmentAddGoal ? Finder::FunctionEnd(AssignmentAddGoal) : 0;
    for (uintptr_t Site = AssignmentAddGoal; Site && Site + 7 <= AssignmentAddGoalEnd && GoalsOffset < 0; ++Site)
    {
        const uint8_t ModRm = Byte(Site + 2);
        const int32_t Displacement = *reinterpret_cast<const int32_t*>(Site + 3);
        if ((Byte(Site) == 0x48 || Byte(Site) == 0x4C) && Byte(Site + 1) == 0x8D && (ModRm & 0xC0) == 0x80 && (ModRm & 0x07) != 0x04 && Displacement > 0 && Displacement < 0x1000)
        {
            GoalsOffset = Displacement;
        }
    }

    if (!FindInList || GoalsOffset < 0)
    {
        Log::Error(L"Finder: AFortAIGoalManager assignment lookup not found");
        return false;
    }
    Natives::AFortAIGoalManager_FindAssignmentInList = reinterpret_cast<decltype(Natives::AFortAIGoalManager_FindAssignmentInList)>(FindInList);
    Natives::UFortAIAssignment_Goals = GoalsOffset;
    Log::Info(L"Finder: UFortAIAssignment goals +0x{:X}", GoalsOffset);
    return true;
}

bool Finder::ResolveGoalDiscouragementLive()
{
    if (Natives::AFortAIGoalManager_GetAssignmentInfo && Natives::FAIAssignmentInfo_DiscouragedGoalTimers >= 0 && Natives::UBTTaskNode_ExecuteTaskIndex >= 0)
    {
        return true;
    }

    void** TestVTable = CdoVTable(L"/Script/FortniteGame.FortQueryTest_GoalFrustrationDiscouragement");
    void** TestBaseVTable = CdoVTable(L"/Script/AIModule.EnvQueryTest");
    uintptr_t Lookup = 0;
    int32_t TimersOffset = -1;
    for (int32_t Index = 1; TestVTable && TestBaseVTable && Index < 0x80 && TimersOffset < 0; ++Index)
    {
        const uintptr_t Function = VTableEntry(TestVTable, Index);
        const uintptr_t FunctionEnd = Function && Function != VTableEntry(TestBaseVTable, Index) ? Finder::FunctionEnd(Function) : 0;
        for (const auto& [Call, Target] : FunctionEnd > Function ? Finder::DirectCalls(Function, FunctionEnd) : std::vector<std::pair<uintptr_t, uintptr_t>>{})
        {
            const uintptr_t TargetEnd = Finder::FunctionStart(Target) == Target ? Finder::FunctionEnd(Target) : 0;
            const uintptr_t First = TargetEnd > Target && TargetEnd - Target < 0x80 ? FindRawCall(Target, TargetEnd) : 0;
            const uintptr_t Reader = First && Byte(TargetEnd - 5) == 0xE9 ? Finder::RelativeTarget(TargetEnd - 5, 1, 5) : 0;
            for (uintptr_t Site = Reader; Site && Site + 4 <= Reader + 0x20 && TimersOffset < 0; ++Site)
            {
                const uint8_t Displacement = Byte(Site + 3);
                const bool bLoadsData = (Byte(Site) == 0x48 || Byte(Site) == 0x4C) && Byte(Site + 1) == 0x8B && (Byte(Site + 2) & 0xC7) == 0x41;
                if (bLoadsData && FindInstruction(Reader, Reader + 0x30, { 0x48, 0x63, 0x41, Displacement + 8 }))
                {
                    TimersOffset = Displacement;
                    Lookup = Finder::RelativeTarget(First, 1, 5);
                }
            }
            if (TimersOffset >= 0)
            {
                break;
            }
        }
    }

    void** TaskVTable = CdoVTable(L"/Script/FortniteGame.FortBTTask_SetFrustrationDiscouragement");
    void** TaskNodeVTable = CdoVTable(L"/Script/AIModule.BTTaskNode");
    int32_t ExecuteTaskIndex = -1;
    for (int32_t Index = 1; TaskVTable && TaskNodeVTable && Index < 0x80 && ExecuteTaskIndex < 0; ++Index)
    {
        const uintptr_t Function = VTableEntry(TaskVTable, Index);
        if (Function && Function != VTableEntry(TaskNodeVTable, Index))
        {
            ExecuteTaskIndex = Index;
        }
    }

    if (!Lookup || TimersOffset < 0 || ExecuteTaskIndex < 0)
    {
        Log::Error(L"Finder: goal discouragement not found (assignment info {}, timers {}, ExecuteTask slot {})", Lookup != 0, TimersOffset, ExecuteTaskIndex);
        return false;
    }
    Natives::AFortAIGoalManager_GetAssignmentInfo = reinterpret_cast<decltype(Natives::AFortAIGoalManager_GetAssignmentInfo)>(Lookup);
    Natives::FAIAssignmentInfo_DiscouragedGoalTimers = TimersOffset;
    Natives::UBTTaskNode_ExecuteTaskIndex = ExecuteTaskIndex;
    Log::Info(L"Finder: AFortAIGoalManager assignment info lookup at 0x{:X}, FAIAssignmentInfo discouraged goal timers +0x{:X}, UBTTaskNode::ExecuteTask slot 0x{:X}", Lookup - Finder::ImageBase(), TimersOffset, ExecuteTaskIndex);
    return true;
}

bool Finder::ResolveQuickBarFocusLive()
{
    // The input binding names the action with a narrow string; the wide "SwitchQuickbar" copies are input-options text.
    uintptr_t Handler = 0;
    for (const uintptr_t Ref : Finder::StringRefs("SwitchQuickbar"))
    {
        for (uintptr_t Site = Ref - 1; Site + 0x20 > Ref && !Handler; --Site)
        {
            if (Byte(Site) == 0x48 && Byte(Site + 1) == 0x8D && Byte(Site + 2) == 0x05)
            {
                Handler = Finder::RelativeTarget(Site, 3, 7);
            }
        }
        if (Handler)
        {
            break;
        }
    }

    const uintptr_t HandlerEnd = IsInText(Handler) ? Finder::FunctionEnd(Handler) : 0;
    // The handler's callee whose first three direct calls go to one function: AFortQuickBars::SwapQuickBarFocus, which
    // focuses through FocusSlot three times before anything else. (1.1 and 1.2 call the swap twice; 1.7.2 once.)
    uintptr_t Swap = 0;
    uintptr_t Focus = 0;
    for (const auto& [Call, Target] : HandlerEnd > Handler ? Finder::DirectCalls(Handler, HandlerEnd) : std::vector<std::pair<uintptr_t, uintptr_t>>{})
    {
        const uintptr_t TargetEnd = Finder::FunctionStart(Target) == Target ? Finder::FunctionEnd(Target) : 0;
        const auto TargetCalls = TargetEnd > Target ? Finder::DirectCalls(Target, TargetEnd) : std::vector<std::pair<uintptr_t, uintptr_t>>{};
        if (TargetCalls.size() >= 3 && TargetCalls[0].second == TargetCalls[1].second && TargetCalls[1].second == TargetCalls[2].second)
        {
            Swap = Target;
            Focus = TargetCalls[0].second;
            break;
        }
    }
    if (!Swap || !Focus)
    {
        Log::Error(L"Finder: AFortQuickBars quickbar focus swap not found (handler {}, swap {})", Handler != 0, Swap != 0);
        return false;
    }
    Natives::AFortQuickBars_SwapQuickBarFocus = reinterpret_cast<decltype(Natives::AFortQuickBars_SwapQuickBarFocus)>(Swap);
    Natives::AFortQuickBars_FocusSlot = reinterpret_cast<decltype(Natives::AFortQuickBars_FocusSlot)>(Focus);
    Log::Info(L"Finder: AFortQuickBars quickbar focus swap at 0x{:X}, slot focus at 0x{:X}", Swap - Finder::ImageBase(), Focus - Finder::ImageBase());
    return true;
}

bool Finder::ResolveSpottingLive(UClass* SpottableActorInterface)
{
    if (Natives::IFortSpottableActorInterface_CanBeSpottedByIndex >= 0 && Natives::UObject_GetInterfaceAddress)
    {
        return true;
    }

    const uintptr_t Ref = OnlyRef("SpotActorTrace");
    uintptr_t InterfaceCall = 0;
    for (uintptr_t Site = Ref ? Ref - 3 : 0; Site && Site + 0x200 > Ref && !InterfaceCall; --Site)
    {
        if (Byte(Site) == 0x41 && Byte(Site + 1) == 0xFF && (Byte(Site + 2) & 0xF8) == 0x50 && Byte(Site + 3) % 8 == 0)
        {
            InterfaceCall = Site;
        }
    }

    uintptr_t GetInterfaceAddress = 0;
    uintptr_t InterfaceClassGetter = 0;
    for (uintptr_t Call = InterfaceCall ? FindRawCall(InterfaceCall - 0x20, InterfaceCall) : 0; Call; Call = FindRawCall(Call + 1, InterfaceCall))
    {
        InterfaceClassGetter = GetInterfaceAddress;
        GetInterfaceAddress = Finder::RelativeTarget(Call, 1, 5);
    }

    const bool bSpottable = IsInText(InterfaceClassGetter) && SpottableActorInterface && reinterpret_cast<UClass* (*)()>(InterfaceClassGetter)() == SpottableActorInterface;
    if (!bSpottable || !GetInterfaceAddress)
    {
        Log::Error(L"Finder: IFortSpottableActorInterface::CanBeSpottedBy not found (call {}, interface {})", InterfaceCall != 0, bSpottable);
        return false;
    }
    if (!Natives::UObject_GetInterfaceAddress)
    {
        Natives::UObject_GetInterfaceAddress = reinterpret_cast<decltype(Natives::UObject_GetInterfaceAddress)>(GetInterfaceAddress);
    }
    Natives::IFortSpottableActorInterface_CanBeSpottedByIndex = Byte(InterfaceCall + 3) / 8;
    Log::Info(L"Finder: IFortSpottableActorInterface::CanBeSpottedBy is slot {}", Natives::IFortSpottableActorInterface_CanBeSpottedByIndex);
    return true;
}

bool Finder::ResolveQueryGeneratorsLive(void** EnemiesVTable)
{
    const uintptr_t Enemies = FunctionOf(L"NULL AIOwner (controller) of Enemies generator.  Cannot generate enemies.");
    int32_t GenerateIndex = -1;
    for (int32_t Index = 0; Enemies && Index < 0x200 && GenerateIndex < 0; ++Index)
    {
        GenerateIndex = VTableEntry(EnemiesVTable, Index) == Enemies ? Index : -1;
    }

    int32_t OwnerOffset = -1;
    for (uintptr_t Site = Enemies; Site && Site + 4 <= Enemies + 0x20 && OwnerOffset < 0; ++Site)
    {
        if (Byte(Site) == 0x48 && Byte(Site + 1) == 0x8D && Byte(Site + 2) == 0x4A)
        {
            OwnerOffset = Byte(Site + 3);
        }
    }

    const uintptr_t AddActor = ExecImplementation(L"/Script/AIModule.EnvQueryGenerator_BlueprintBase:AddGeneratedActor");
    const uintptr_t AddActorEnd = AddActor ? Finder::FunctionEnd(AddActor) : 0;
    uintptr_t AddItem = 0;
    for (uintptr_t Call = AddActorEnd ? FindRawCall(AddActor, AddActorEnd) : 0; Call && !AddItem; Call = FindRawCall(Call + 1, AddActorEnd))
    {
        const uintptr_t Target = Finder::RelativeTarget(Call, 1, 5);
        for (uintptr_t Site = Target; Site + 7 <= Target + 0x40 && !AddItem; ++Site)
        {
            if (Byte(Site) == 0x0F && Byte(Site + 1) == 0xB7 && (Byte(Site + 2) & 0xC7) == 0x81)
            {
                AddItem = Target;
            }
        }
    }

    int32_t ValueSizeOffset = -1;
    int32_t RawDataOffset = -1;
    for (uintptr_t Site = AddItem; Site && Site + 7 <= AddItem + 0x60; ++Site)
    {
        if (ValueSizeOffset < 0 && Byte(Site) == 0x0F && Byte(Site + 1) == 0xB7 && (Byte(Site + 2) & 0xC0) == 0x80 && (Byte(Site + 2) & 0x07) == 0x01)
        {
            ValueSizeOffset = *reinterpret_cast<const int32_t*>(Site + 3);
        }
        if (RawDataOffset < 0 && Byte(Site) == 0x48 && Byte(Site + 1) == 0x83 && Byte(Site + 2) == 0xC1)
        {
            RawDataOffset = Byte(Site + 3);
        }
    }

    if (GenerateIndex < 0 || OwnerOffset < 0 || !AddItem || ValueSizeOffset < 0 || RawDataOffset < 0)
    {
        Log::Error(L"Finder: EQS goal generators not found (slot {}, owner {}, add {}, value size {}, raw data {})", GenerateIndex, OwnerOffset, AddItem != 0, ValueSizeOffset, RawDataOffset);
        return false;
    }
    Natives::UEnvQueryGenerator_GenerateItemsIndex = GenerateIndex;
    Natives::FEnvQueryInstance_Owner = OwnerOffset;
    Natives::FEnvQueryInstance_AddItemData = reinterpret_cast<decltype(Natives::FEnvQueryInstance_AddItemData)>(AddItem);
    Natives::FEnvQueryInstance_ValueSize = ValueSizeOffset;
    Natives::FEnvQueryInstance_RawData = RawDataOffset;
    Log::Info(L"Finder: UEnvQueryGenerator::GenerateItems is slot 0x{:X}, query owner +0x{:X}, raw data +0x{:X}, value size +0x{:X}", GenerateIndex, OwnerOffset, RawDataOffset, ValueSizeOffset);
    return true;
}

bool Finder::ResolveQueryContextsLive(void** EncounterPrimaryAssignmentGoalsVTable, UClass* GoalItemType)
{
    const uintptr_t ProvideContext = FunctionOf(L"Unable to find encounter info for UFortQueryContext_EncounterPrimaryAssignmentGoals.  Owner is %s");
    const uintptr_t ProvideContextEnd = ProvideContext ? Finder::FunctionEnd(ProvideContext) : 0;
    int32_t ProvideContextIndex = -1;
    for (int32_t Index = 0; ProvideContext && Index < 0x200 && ProvideContextIndex < 0; ++Index)
    {
        ProvideContextIndex = VTableEntry(EncounterPrimaryAssignmentGoalsVTable, Index) == ProvideContext ? Index : -1;
    }

    uintptr_t GetInterfaceAddress = 0;
    int32_t GetEncounterInfoIndex = -1;
    uintptr_t SetContextHelper = 0;
    for (const auto& [Call, Target] : ProvideContextEnd ? Finder::DirectCalls(ProvideContext, ProvideContextEnd) : std::vector<std::pair<uintptr_t, uintptr_t>>{})
    {
        if (!GetInterfaceAddress && FindInstruction(Target, Target + 0x40, { 0x8B, -1, 0xB4, 0x00, 0x00, 0x00, 0xC1, 0xE8, 0x0E }))
        {
            GetInterfaceAddress = Target;
            for (uintptr_t Site = Call + 5; Site < Call + 0x18 && GetEncounterInfoIndex < 0; ++Site)
            {
                const uint8_t ModRm = Byte(Site + 1);
                if (Byte(Site) == 0xFF && (ModRm & 0xF8) == 0x50 && (ModRm & 0x07) != 0x04 && Byte(Site + 2) % 8 == 0)
                {
                    GetEncounterInfoIndex = Byte(Site + 2) / 8;
                }
            }
        }

        const uintptr_t TypeCall = SetContextHelper ? 0 : FindRawCall(Target, Target + 0x20);
        const uintptr_t TypeGetter = TypeCall ? Finder::RelativeTarget(TypeCall, 1, 5) : 0;
        const uintptr_t CachedType = TypeGetter ? FindInstruction(TypeGetter, TypeGetter + 0x10, { 0x48, 0x83, 0x3D, -1, -1, -1, -1, 0x00 }) : 0;
        if (CachedType && GoalItemType && *reinterpret_cast<UClass* const*>(Finder::RelativeTarget(CachedType, 3, 8)) == GoalItemType)
        {
            SetContextHelper = Target;
        }
    }

    if (ProvideContextIndex < 0 || !GetInterfaceAddress || GetEncounterInfoIndex < 0 || !SetContextHelper)
    {
        Log::Error(L"Finder: EQS goal contexts not found (slot {}, interface {}, encounter info slot {}, context helper {})", ProvideContextIndex, GetInterfaceAddress != 0, GetEncounterInfoIndex, SetContextHelper != 0);
        return false;
    }
    Natives::UEnvQueryContext_ProvideContextIndex = ProvideContextIndex;
    Natives::UObject_GetInterfaceAddress = reinterpret_cast<decltype(Natives::UObject_GetInterfaceAddress)>(GetInterfaceAddress);
    Natives::IFortAIEncounterInfoOwnerInterface_GetEncounterInfoIndex = GetEncounterInfoIndex;
    Natives::UFortQueryItemType_Goal_SetContextHelper = reinterpret_cast<decltype(Natives::UFortQueryItemType_Goal_SetContextHelper)>(SetContextHelper);
    Log::Info(L"Finder: UEnvQueryContext::ProvideContext is slot 0x{:X}, IFortAIEncounterInfoOwnerInterface::GetEncounterInfo slot 0x{:X}", ProvideContextIndex, GetEncounterInfoIndex);
    return true;
}

bool Finder::ResolveMcpProfileWorldLive(void** ProfileVTable)
{
    const uintptr_t Bind = FunctionOf(L"ProfileWorld::BindToPlayerController: Called with existing inventory for %s, reinitializing inventory");
    const uintptr_t BindEnd = Bind ? Finder::FunctionEnd(Bind) : 0;
    int32_t BindIndex = -1;
    for (int32_t Index = 0; Bind && Index < 0x400 && BindIndex < 0; ++Index)
    {
        BindIndex = VTableEntry(ProfileVTable, Index) == Bind ? Index : -1;
    }

    int32_t InventoryOffset = -1;
    for (uintptr_t Site = Bind; Site && Site + 7 <= BindEnd && InventoryOffset < 0; ++Site)
    {
        const uint8_t ModRm = Byte(Site + 2);
        if ((Byte(Site) == 0x48 || Byte(Site) == 0x4C) && Byte(Site + 1) == 0x89 && (ModRm & 0xC0) == 0x80 && (ModRm & 0x07) != 0x04)
        {
            InventoryOffset = *reinterpret_cast<const int32_t*>(Site + 3);
        }
    }

    int32_t InitializedOffset = -1;
    for (int32_t Index = 0; InventoryOffset > 0 && Index < 0x100 && InitializedOffset < 0; ++Index)
    {
        const uintptr_t Function = VTableEntry(ProfileVTable, Index);
        const uintptr_t FunctionEnd = Function ? Finder::FunctionEnd(Function) : 0;
        for (uintptr_t Site = Function; Site && FunctionEnd - Function < 0x4000 && Site + 14 <= FunctionEnd && InitializedOffset < 0; ++Site)
        {
            const uint8_t ClearModRm = Byte(Site + 1);
            const uint8_t StoreModRm = Byte(Site + 9);
            if (Byte(Site) == 0xC6 && (ClearModRm & 0xF8) == 0x80 && (ClearModRm & 0x07) != 0x04 && Byte(Site + 6) == 0x00
                && (Byte(Site + 7) == 0x48 || Byte(Site + 7) == 0x4C) && Byte(Site + 8) == 0x89 && (StoreModRm & 0xC0) == 0x80
                && *reinterpret_cast<const int32_t*>(Site + 10) == InventoryOffset)
            {
                InitializedOffset = *reinterpret_cast<const int32_t*>(Site + 2);
            }
        }
    }

    if (BindIndex < 0 || InventoryOffset <= 0 || InitializedOffset <= 0)
    {
        Log::Error(L"Finder: UFortMcpProfileWorld inventory binding not found (bind slot {}, inventory {}, initialized {})", BindIndex, InventoryOffset, InitializedOffset);
        return false;
    }
    Natives::UFortMcpProfileWorld_BindToPlayerControllerIndex = BindIndex;
    Natives::UFortMcpProfileWorld_Inventory = InventoryOffset;
    Natives::UFortMcpProfileWorld_bIsInventoryInitialized = InitializedOffset;
    Log::Info(L"Finder: UFortMcpProfileWorld::BindToPlayerController is slot 0x{:X}, inventory +0x{:X}, bIsInventoryInitialized +0x{:X}", BindIndex, InventoryOffset, InitializedOffset);
    return true;
}

bool Finder::ResolveWorldItemLive(void** InventoryOwnerVTable)
{
    int32_t LoadedAmmoIndex = -1;
    for (int32_t Index = 0; InventoryOwnerVTable && Index < 0x40 && LoadedAmmoIndex < 0; ++Index)
    {
        const uintptr_t Function = VTableEntry(InventoryOwnerVTable, Index);
        const uintptr_t FunctionEnd = Function ? Finder::FunctionEnd(Function) : 0;
        if (!FunctionEnd || FunctionEnd - Function > 0x100 || !FindInstruction(Function, FunctionEnd, { 0x80, 0xB9, -1, -1, -1, -1, 0x03 }))
        {
            continue;
        }

        for (uintptr_t Site = FindVirtualCall(Function, FunctionEnd); Site; Site = FindVirtualCall(Site + 1, FunctionEnd))
        {
            if ((Byte(Site + 1) & 0x38) == 0x20)
            {
                LoadedAmmoIndex = VirtualSlot(Site);
                break;
            }
        }
    }

    if (LoadedAmmoIndex < 0)
    {
        Log::Error(L"Finder: UFortWorldItem::SetLoadedAmmo slot not found");
        return false;
    }
    Natives::UFortWorldItem_SetLoadedAmmoIndex = LoadedAmmoIndex;
    Log::Info(L"Finder: UFortWorldItem::SetLoadedAmmo is slot 0x{:X}", LoadedAmmoIndex);
    return true;
}

uint8_t* Finder::NullGetCurrentCall(UFunction* Function)
{
    const uintptr_t Exec = Function ? reinterpret_cast<uintptr_t>(Function->GetNativeFunc()) : 0;
    const uintptr_t ExecEnd = IsInText(Exec) ? Finder::FunctionEnd(Exec) : 0;
    uintptr_t Implementation = 0;
    for (uintptr_t Call = ExecEnd ? FindRawCall(Exec, ExecEnd) : 0; Call; Call = FindRawCall(Call + 1, ExecEnd))
    {
        Implementation = Finder::RelativeTarget(Call, 1, 5);
    }

    const uintptr_t ImplementationEnd = Implementation ? Finder::FunctionEnd(Implementation) : 0;
    for (uintptr_t Call = ImplementationEnd ? FindRawCall(Implementation, ImplementationEnd) : 0; Call; Call = FindRawCall(Call + 1, ImplementationEnd))
    {
        const uintptr_t Callee = Finder::RelativeTarget(Call, 1, 5);
        if (Byte(Callee) == 0x33 && Byte(Callee + 1) == 0xC0 && Byte(Callee + 2) == 0xC3 && Call >= 6 && (Byte(Call - 6) == 0xFF || Byte(Call - 9) == 0xFF))
        {
            return reinterpret_cast<uint8_t*>(Call);
        }
    }
    return nullptr;
}

bool Finder::ResolveCraftingLive()
{
    if (Natives::UFortSchematicItem_CanCraftSchematic && Natives::AFortPlayerController_GetSchematicForItem && Natives::UFortSchematicItem_ServerCraftSchematicIndex >= 0)
    {
        return true;
    }

    const auto LastCall = [](uintptr_t Function)
    {
        const uintptr_t FunctionEnd = IsInText(Function) ? Finder::FunctionEnd(Function) : 0;
        uintptr_t Target = 0;
        for (uintptr_t Call = FunctionEnd ? FindRawCall(Function, FunctionEnd) : 0; Call; Call = FindRawCall(Call + 1, FunctionEnd))
        {
            Target = Finder::RelativeTarget(Call, 1, 5);
        }
        return Target;
    };
    const auto ExecOf = [](const wchar_t* FunctionPath)
    {
        UFunction* Function = FindObject<UFunction>(nullptr, FunctionPath);
        return Function ? reinterpret_cast<uintptr_t>(Function->GetNativeFunc()) : 0;
    };

    const uintptr_t ServerCraft = FunctionOf(L"Cannot get SchematicItem for schematic crafting");
    const uintptr_t ServerCraftEnd = ServerCraft ? Finder::FunctionEnd(ServerCraft) : 0;
    const int32_t CraftIndex = ServerCraftEnd ? VirtualSlot(FindVirtualCall(ServerCraft, ServerCraftEnd)) : -1;

    const uintptr_t InventoryContextCanCraft = LastCall(ExecOf(L"/Script/FortniteUI.FortInventoryContext:CanCraftSchematic"));
    const uintptr_t CanCraft = InventoryContextCanCraft ? LastCall(InventoryContextCanCraft) : 0;
    const uintptr_t QuantityProduced = LastCall(ExecOf(L"/Script/FortniteGame.FortSchematicItem:GetSchematicQuantityProduced"));

    const uintptr_t CanCraftEnd = CanCraft && QuantityProduced ? Finder::FunctionEnd(CanCraft) : 0;
    uintptr_t Previous = 0;
    uintptr_t GetSchematicForItem = 0;
    for (uintptr_t Call = CanCraftEnd ? FindRawCall(CanCraft, CanCraftEnd) : 0; Call && !GetSchematicForItem; Call = FindRawCall(Call + 1, CanCraftEnd))
    {
        const uintptr_t Target = Finder::RelativeTarget(Call, 1, 5);
        if (Target == QuantityProduced)
        {
            GetSchematicForItem = Previous;
        }
        Previous = Target;
    }

    if (CraftIndex < 0 || !CanCraft || !GetSchematicForItem)
    {
        Log::Error(L"Finder: schematic crafting not found");
        return false;
    }
    Natives::UFortSchematicItem_ServerCraftSchematicIndex = CraftIndex;
    Natives::UFortSchematicItem_CanCraftSchematic = reinterpret_cast<decltype(Natives::UFortSchematicItem_CanCraftSchematic)>(CanCraft);
    Natives::AFortPlayerController_GetSchematicForItem = reinterpret_cast<decltype(Natives::AFortPlayerController_GetSchematicForItem)>(GetSchematicForItem);
    Log::Info(L"Finder: UFortSchematicItem::ServerCraftSchematic is slot 0x{:X}", CraftIndex);
    return true;
}

bool Finder::ResolveObjectiveActivationLive(ptrdiff_t MissionActiveObjectivesOffset)
{
    const uintptr_t StartPlaying = FunctionOf(L"Mission %s has no Active Objectives! Did you set up the Mission Info correctly? Set to Silent Destroy!");
    const uintptr_t StartPlayingEnd = StartPlaying ? Finder::FunctionEnd(StartPlaying) : 0;
    const int32_t Offset = static_cast<int32_t>(MissionActiveObjectivesOffset);
    int32_t ActivateIndex = -1;
    for (uintptr_t Call = StartPlayingEnd && Offset > 0 ? FindRawCall(StartPlaying, StartPlayingEnd) : 0; Call && ActivateIndex < 0; Call = FindRawCall(Call + 1, StartPlayingEnd))
    {
        const uintptr_t Target = Finder::RelativeTarget(Call, 1, 5);
        const uintptr_t TargetEnd = Finder::FunctionEnd(Target);
        if (!TargetEnd || TargetEnd - Target > 0x100 || !FindInstruction(Target, TargetEnd, { Offset & 0xFF, (Offset >> 8) & 0xFF, (Offset >> 16) & 0xFF, (Offset >> 24) & 0xFF }))
        {
            continue;
        }

        const uintptr_t Site = FindVirtualCall(Target, TargetEnd);
        if (Site && !FindVirtualCall(Site + 1, TargetEnd))
        {
            ActivateIndex = VirtualSlot(Site);
        }
    }

    if (ActivateIndex < 0)
    {
        Log::Error(L"Finder: AFortObjectiveBase::ActivateObjective slot not found");
        return false;
    }
    Natives::AFortObjectiveBase_ActivateObjectiveIndex = ActivateIndex;
    Log::Info(L"Finder: AFortObjectiveBase::ActivateObjective is slot 0x{:X}", ActivateIndex);
    return true;
}

bool Finder::ResolveMissionObjectivesLive()
{
    const uintptr_t ObjectiveStatusUpdated = FunctionOf(L"Mission %s: Objective %s status is now %s");
    void** MissionVTable = CdoVTable(L"/Script/FortniteGame.FortMission");
    int32_t StatusUpdatedIndex = -1;
    for (int32_t Index = 0; MissionVTable && ObjectiveStatusUpdated && Index < 0x400 && StatusUpdatedIndex < 0; ++Index)
    {
        const uintptr_t Entry = VTableEntry(MissionVTable, Index);
        if (!Entry)
        {
            break;
        }
        if (Entry == ObjectiveStatusUpdated)
        {
            StatusUpdatedIndex = Index;
        }
    }

    if (StatusUpdatedIndex < 0)
    {
        Log::Error(L"Finder: AFortMission::ObjectiveStatusUpdated slot not found");
        return false;
    }
    Natives::AFortMission_ObjectiveStatusUpdatedIndex = StatusUpdatedIndex;
    Log::Info(L"Finder: AFortMission::ObjectiveStatusUpdated is slot 0x{:X}", StatusUpdatedIndex);
    return true;
}

bool Finder::ResolveMissionEventLive()
{
    const uintptr_t CallHandleMissionEventFunction = FunctionOf(L"HandleMissionEvent_%s");
    void** MissionVTable = CdoVTable(L"/Script/FortniteGame.FortMission");
    int32_t CustomHandleIndex = -1;
    int32_t Matches = 0;
    for (const uintptr_t Site : CallHandleMissionEventFunction && MissionVTable ? Finder::CallsTo(CallHandleMissionEventFunction, false) : std::vector<uintptr_t>())
    {
        const uintptr_t VirtualCall = FindVirtualCall(Site - 0x10, Site);
        const int32_t Slot = VirtualCall && Byte(VirtualCall + 1) == 0x90 ? VirtualSlot(VirtualCall) : -1;
        const uintptr_t Entry = VTableEntry(MissionVTable, Slot);
        if (Entry && reinterpret_cast<void*>(Entry) != Natives::SharedEmptyFunction)
        {
            CustomHandleIndex = Slot;
            ++Matches;
        }
    }

    if (Matches != 1)
    {
        Log::Error(L"Finder: AFortMission::CustomHandleMissionEvent slot not found ({} matches)", Matches);
        return false;
    }
    Natives::AFortMission_CustomHandleMissionEventIndex = CustomHandleIndex;
    Log::Info(L"Finder: AFortMission::CustomHandleMissionEvent is slot 0x{:X}", CustomHandleIndex);
    return true;
}

bool Finder::ResolveMissionParTimeLive(uintptr_t CustomHandleMissionEvent)
{
    if (Natives::AFortMission_HandleDeferredMissionEventIndex >= 0)
    {
        return true;
    }

    const uintptr_t End = CustomHandleMissionEvent ? Finder::FunctionEnd(CustomHandleMissionEvent) : 0;
    int32_t DeferredIndex = -1;
    int32_t Matches = 0;
    for (uintptr_t Site = CustomHandleMissionEvent; End && Site + 7 <= End; ++Site)
    {
        if (Byte(Site) != 0x48 || Byte(Site + 1) != 0x8D || (Byte(Site + 2) & 0xC7) != 0x05)
        {
            continue;
        }
        const uintptr_t Thunk = Finder::RelativeTarget(Site, 3, 7);
        if (IsInText(Thunk) && Byte(Thunk) == 0x48 && Byte(Thunk + 1) == 0x8B && Byte(Thunk + 2) == 0x01 && Byte(Thunk + 3) == 0xFF && Byte(Thunk + 4) == 0xA0)
        {
            const int32_t Displacement = *reinterpret_cast<const int32_t*>(Thunk + 5);
            DeferredIndex = Displacement % 8 == 0 ? Displacement / 8 : -1;
            ++Matches;
        }
    }

    if (Matches != 1 || DeferredIndex < 0)
    {
        Log::Error(L"Finder: AFortMission deferred mission event slot not found ({} matches)", Matches);
        return false;
    }
    Natives::AFortMission_HandleDeferredMissionEventIndex = DeferredIndex;
    Log::Info(L"Finder: AFortMission deferred mission event handler is slot 0x{:X}", DeferredIndex);
    return true;
}

// AFortMission's badge grant (grants the badge to every player through the mission manager's EarnedBadgesArray add,
// then spawns a basic client announcement): the only AFortMission slot not inherited from AActor whose callee's callee
// passes the mission manager's EarnedBadgesArray in rcx.
bool Finder::ResolveMissionBadgeLive(ptrdiff_t EarnedBadgesArrayOffset)
{
    if (Natives::AFortMission_GrantBadgeIndex >= 0)
    {
        return true;
    }

    const int32_t Offset = static_cast<int32_t>(EarnedBadgesArrayOffset);
    auto PassesBadgeArray = [Offset](uintptr_t Function)
    {
        const uintptr_t End = Finder::FunctionEnd(Function);
        for (uintptr_t Site = Function; End && End - Function < 0x1000 && Site + 7 <= End; ++Site)
        {
            if ((Byte(Site) == 0x48 || Byte(Site) == 0x49) && Byte(Site + 1) == 0x8D && (Byte(Site + 2) & 0xF8) == 0x88 && (Byte(Site + 2) & 0x07) != 0x04 && *reinterpret_cast<const int32_t*>(Site + 3) == Offset)
            {
                return true;
            }
        }
        return false;
    };

    void** MissionVTable = CdoVTable(L"/Script/FortniteGame.FortMission");
    void** ActorVTable = CdoVTable(L"/Script/Engine.Actor");
    int32_t BadgeIndex = -1;
    int32_t Matches = 0;
    for (int32_t Index = 0; MissionVTable && ActorVTable && Offset > 0 && Index < 0x400; ++Index)
    {
        const uintptr_t Entry = VTableEntry(MissionVTable, Index);
        if (!Entry)
        {
            break;
        }
        if (Entry == VTableEntry(ActorVTable, Index) || reinterpret_cast<void*>(Entry) == Natives::SharedEmptyFunction)
        {
            continue;
        }

        const uintptr_t EntryEnd = Finder::FunctionEnd(Entry);
        bool bMatch = false;
        for (const auto& [Site, Target] : EntryEnd && EntryEnd - Entry < 0x1000 ? Finder::DirectCalls(Entry, EntryEnd) : std::vector<std::pair<uintptr_t, uintptr_t>>())
        {
            const uintptr_t TargetEnd = IsInText(Target) ? Finder::FunctionEnd(Target) : 0;
            for (const auto& [InnerSite, Inner] : TargetEnd && TargetEnd - Target < 0x1000 ? Finder::DirectCalls(Target, TargetEnd) : std::vector<std::pair<uintptr_t, uintptr_t>>())
            {
                bMatch = bMatch || (IsInText(Inner) && PassesBadgeArray(Inner));
            }
        }
        if (bMatch)
        {
            BadgeIndex = Index;
            ++Matches;
        }
    }

    if (Matches != 1)
    {
        Log::Error(L"Finder: AFortMission badge grant slot not found ({} matches)", Matches);
        return false;
    }
    Natives::AFortMission_GrantBadgeIndex = BadgeIndex;
    Log::Info(L"Finder: AFortMission badge grant is slot 0x{:X}", BadgeIndex);
    return true;
}

bool Finder::ResolveTheaterMapDataLive()
{
    if (Natives::AFortWorldManager_GetTheaterMapData)
    {
        return true;
    }

    const uintptr_t Function = FunctionOf(L"ZoneInfo Invalid!");
    if (!Function)
    {
        Log::Error(L"Finder: AFortWorldManager::GetTheaterMapData not found");
        return false;
    }
    Natives::AFortWorldManager_GetTheaterMapData = reinterpret_cast<decltype(Natives::AFortWorldManager_GetTheaterMapData)>(Function);
    Log::Info(L"Finder: AFortWorldManager::GetTheaterMapData found");
    return true;
}

bool Finder::ResolveHomebaseGadgetsLive()
{
    if (Natives::UFortMcpProfileAccount_GetModeLoadout && Natives::UFortRegisteredPlayerInfo_GetAbilitySystemInterface && Natives::UFortHomebaseManager_GetSelectableGadgetItems && Natives::UFortHomebaseManager_GetOwnedNodeAbilityKits)
    {
        return true;
    }
    if (!Natives::FFrame_Step && !Finder::ResolveScriptFrame())
    {
        return false;
    }

    const auto ImplementationOf = [](const wchar_t* FunctionPath)
    {
        const uintptr_t Call = reinterpret_cast<uintptr_t>(Finder::ImplementationCall(FindObject<UFunction>(nullptr, FunctionPath)));
        return Call ? Finder::RelativeTarget(Call, 1, 5) : 0;
    };
    const uintptr_t OwnedNodes = ImplementationOf(L"/Script/FortniteGame.FortHomebaseManager:GetOwnedNodes");
    const uintptr_t SelectableGadgets = ImplementationOf(L"/Script/FortniteUI.FortFrontEndContext:GetSelectableGadgets");
    const uintptr_t SelectableGadgetsEnd = OwnedNodes && IsInText(SelectableGadgets) ? Finder::FunctionEnd(SelectableGadgets) : 0;

    uintptr_t AbilitySystemInterface = 0;
    uintptr_t GadgetItems = 0;
    uintptr_t OwnedNodeKits = 0;
    uintptr_t Previous = 0;
    for (uintptr_t Call = SelectableGadgetsEnd ? FindRawCall(SelectableGadgets, SelectableGadgetsEnd) : 0; Call && !GadgetItems; Call = FindRawCall(Call + 1, SelectableGadgetsEnd))
    {
        const uintptr_t Target = Finder::RelativeTarget(Call, 1, 5);
        const uintptr_t First = FindRawCall(Target, Target + 0x80);
        const uintptr_t FirstTarget = First ? Finder::RelativeTarget(First, 1, 5) : 0;
        if (FirstTarget && CallWithin(FirstTarget, 0x40, OwnedNodes))
        {
            AbilitySystemInterface = Previous;
            GadgetItems = Target;
            OwnedNodeKits = FirstTarget;
        }
        Previous = Target;
    }

    const std::vector<uintptr_t> SettingGadgetRefs = Finder::StringRefs(L"Setting Gadget %d for local player. Gadget Name: %s");
    const uintptr_t SettingGadget = SettingGadgetRefs.empty() ? 0 : *std::min_element(SettingGadgetRefs.begin(), SettingGadgetRefs.end());
    const uintptr_t SyncGadgets = Finder::FunctionStart(SettingGadget);
    uintptr_t ModeLoadout = 0;
    for (uintptr_t Call = SyncGadgets ? FindRawCall(SyncGadgets, SettingGadget) : 0; Call; Call = FindRawCall(Call + 1, SettingGadget))
    {
        ModeLoadout = Finder::RelativeTarget(Call, 1, 5);
    }
    if (ModeLoadout && !(SelectableGadgetsEnd && CallWithin(SelectableGadgets, SelectableGadgetsEnd - SelectableGadgets, ModeLoadout)))
    {
        ModeLoadout = 0;
    }

    if (!AbilitySystemInterface || !GadgetItems || !ModeLoadout)
    {
        Log::Error(L"Finder: homebase gadgets not found (owned nodes {}, selectable gadgets {}, gadget items {}, mode loadout {})", OwnedNodes != 0, SelectableGadgets != 0, GadgetItems != 0, ModeLoadout != 0);
        return false;
    }
    Natives::UFortMcpProfileAccount_GetModeLoadout = reinterpret_cast<decltype(Natives::UFortMcpProfileAccount_GetModeLoadout)>(ModeLoadout);
    Natives::UFortRegisteredPlayerInfo_GetAbilitySystemInterface = reinterpret_cast<decltype(Natives::UFortRegisteredPlayerInfo_GetAbilitySystemInterface)>(AbilitySystemInterface);
    Natives::UFortHomebaseManager_GetSelectableGadgetItems = reinterpret_cast<decltype(Natives::UFortHomebaseManager_GetSelectableGadgetItems)>(GadgetItems);
    Natives::UFortHomebaseManager_GetOwnedNodeAbilityKits = reinterpret_cast<decltype(Natives::UFortHomebaseManager_GetOwnedNodeAbilityKits)>(OwnedNodeKits);
    Log::Info(L"Finder: homebase gadgets found");
    return true;
}

bool Finder::ResolveLoginLive()
{
    const uintptr_t GameModeBaseLogin = FunctionOf(L"Login: Couldn't spawn player controller of class %s");
    void** GameModeBaseVTable = CdoVTable(L"/Script/Engine.GameModeBase");
    int32_t LoginIndex = -1;
    for (int32_t Index = 0; GameModeBaseVTable && GameModeBaseLogin && Index < 0x400 && LoginIndex < 0; ++Index)
    {
        const uintptr_t Entry = VTableEntry(GameModeBaseVTable, Index);
        if (!Entry)
        {
            break;
        }
        if (Entry == GameModeBaseLogin)
        {
            LoginIndex = Index;
        }
    }
    if (LoginIndex < 0)
    {
        Log::Error(L"Finder: AGameModeBase::Login slot not found");
        return false;
    }
    Natives::AGameModeBase_LoginIndex = LoginIndex;
    Log::Info(L"Finder: AGameModeBase::Login is slot 0x{:X}", LoginIndex);
    return true;
}

bool Finder::ResolveAIDirectorLive()
{
    void** ZoneVTable = CdoVTable(L"/Script/FortniteGame.FortGameModeZone");
    const uintptr_t CreateAIGoalManager = FunctionOf(L"nullptr AI Goal Manager class specified by Game Data; created default AI Goal manager anyway, but some settings may be missing.");
    int32_t DirectorIndex = -1;
    for (int32_t Index = 0; ZoneVTable && CreateAIGoalManager && Index < 0x400 && DirectorIndex < 0; ++Index)
    {
        const uintptr_t Function = VTableEntry(ZoneVTable, Index);
        const uintptr_t FunctionEnd = Function ? Finder::FunctionEnd(Function) : 0;
        int32_t PreviousSlot = -1;
        for (uintptr_t Site = FunctionEnd > Function && FunctionEnd - Function < 0x100 ? FindVirtualCall(Function, FunctionEnd) : 0; Site; Site = FindVirtualCall(Site + 1, FunctionEnd))
        {
            const int32_t Slot = VirtualSlot(Site);
            if (Slot >= 0 && VTableEntry(ZoneVTable, Slot) == CreateAIGoalManager && PreviousSlot >= 0 && reinterpret_cast<void*>(VTableEntry(ZoneVTable, PreviousSlot)) == Natives::SharedEmptyFunction)
            {
                DirectorIndex = PreviousSlot;
                break;
            }
            PreviousSlot = Slot;
        }
    }

    if (DirectorIndex < 0)
    {
        Log::Error(L"Finder: AFortGameModeZone::CreateAIDirector slot not found");
        return false;
    }
    Natives::AFortGameModeZone_CreateAIDirectorIndex = DirectorIndex;
    Log::Info(L"Finder: AFortGameModeZone::CreateAIDirector is slot 0x{:X}", DirectorIndex);
    return true;
}

bool Finder::ResolveMissionManagerLive(ptrdiff_t GameStateMissionManagerOffset)
{
    const uintptr_t GameModeHandleMatchHasStarted = FunctionOf(L"BugLoc");
    void** GameModeVTable = CdoVTable(L"/Script/Engine.GameMode");
    void** FortGameModeVTable = CdoVTable(L"/Script/FortniteGame.FortGameMode");
    const int32_t HandleMatchHasStartedIndex = GameModeVTable && GameModeHandleMatchHasStarted ? Finder::VTableIndex(GameModeVTable, reinterpret_cast<void*>(GameModeHandleMatchHasStarted), 0x400) : -1;
    const uintptr_t FortHandleMatchHasStarted = VTableEntry(FortGameModeVTable, HandleMatchHasStartedIndex);
    const uintptr_t FortHandleMatchHasStartedEnd = FortHandleMatchHasStarted ? Finder::FunctionEnd(FortHandleMatchHasStarted) : 0;

    const auto StoresMissionManager = [GameStateMissionManagerOffset](uintptr_t Function)
    {
        const uintptr_t FunctionEnd = IsInText(Function) ? Finder::FunctionEnd(Function) : 0;
        for (uintptr_t Store = FunctionEnd ? FindInstruction(Function, FunctionEnd, { 0x48, 0x89 }) : 0; Store; Store = FindInstruction(Store + 1, FunctionEnd, { 0x48, 0x89 }))
        {
            const uint8_t ModRm = Byte(Store + 2);
            if ((ModRm & 0xC0) == 0x80 && (ModRm & 0x07) != 0x04 && *reinterpret_cast<const int32_t*>(Store + 3) == GameStateMissionManagerOffset)
            {
                return true;
            }
        }
        return false;
    };

    // The record constructor, then AFortGameState::SpawnMissionManager: a direct call on 1.1 and 1.2, a virtual one on
    // 1.7.2 (0x57BA13, AFortGameState slot 0xDE), resolved through the game state's vtable.
    void** GameStateVTable = CdoVTable(L"/Script/FortniteGame.FortGameState");
    uintptr_t RecordConstructor = 0;
    uintptr_t SpawnMissionManager = 0;
    uintptr_t PreviousCallee = 0;
    for (uintptr_t Call = FortHandleMatchHasStartedEnd && GameStateMissionManagerOffset > 0 ? FindRawCall(FortHandleMatchHasStarted, FortHandleMatchHasStartedEnd) : 0; Call && !SpawnMissionManager; Call = FindRawCall(Call + 1, FortHandleMatchHasStartedEnd))
    {
        const uintptr_t Callee = Finder::RelativeTarget(Call, 1, 5);
        if (Callee == GameModeHandleMatchHasStarted)
        {
            break;
        }
        if (StoresMissionManager(Callee))
        {
            SpawnMissionManager = Callee;
            RecordConstructor = PreviousCallee;
            break;
        }

        const uintptr_t Next = FindRawCall(Call + 5, FortHandleMatchHasStartedEnd);
        const uintptr_t Virtual = FindVirtualCall(Call + 5, Next ? Next : FortHandleMatchHasStartedEnd);
        const uintptr_t Slot = Virtual && Byte(Virtual + 1) == 0x90 ? VTableEntry(GameStateVTable, VirtualSlot(Virtual)) : 0;
        if (Slot && StoresMissionManager(Slot))
        {
            SpawnMissionManager = Slot;
            RecordConstructor = Callee;
            break;
        }
        PreviousCallee = Callee;
    }

    if (!SpawnMissionManager || !RecordConstructor)
    {
        Log::Error(L"Finder: AFortGameState::SpawnMissionManager / FFortMissionManagerRecord::FFortMissionManagerRecord not found");
        return false;
    }
    Natives::AFortGameState_SpawnMissionManager = reinterpret_cast<decltype(Natives::AFortGameState_SpawnMissionManager)>(SpawnMissionManager);
    Natives::FFortMissionManagerRecord_FFortMissionManagerRecord = reinterpret_cast<decltype(Natives::FFortMissionManagerRecord_FFortMissionManagerRecord)>(RecordConstructor);
    Log::Info(L"Finder: AFortGameState::SpawnMissionManager and FFortMissionManagerRecord::FFortMissionManagerRecord found");
    return true;
}

bool Finder::ResolveWorldItemDefinitionLive()
{
    UFunction* EquipWeaponDefinition = FindObject<UFunction>(nullptr, L"/Script/FortniteGame.FortPawn:EquipWeaponDefinition");
    const uintptr_t Exec = EquipWeaponDefinition ? reinterpret_cast<uintptr_t>(EquipWeaponDefinition->GetNativeFunc()) : 0;
    const uintptr_t ExecEnd = IsInText(Exec) ? Finder::FunctionEnd(Exec) : 0;
    uintptr_t Equip = 0;
    for (uintptr_t Call = ExecEnd ? FindRawCall(Exec, ExecEnd) : 0; Call; Call = FindRawCall(Call + 1, ExecEnd))
    {
        Equip = Finder::RelativeTarget(Call, 1, 5);
    }

    int32_t ServerExecuteIndex = -1;
    void** VTable = CdoVTable(L"/Script/FortniteGame.FortWeaponItemDefinition");
    for (int32_t Index = 0; VTable && Equip && Index < 0x100 && ServerExecuteIndex < 0; ++Index)
    {
        const uintptr_t Function = VTableEntry(VTable, Index);
        const uintptr_t FunctionEnd = Function ? Finder::FunctionEnd(Function) : 0;
        if (FunctionEnd > Function && FunctionEnd - Function < 0x400 && CallWithin(Function, FunctionEnd - Function, Equip))
        {
            ServerExecuteIndex = Index;
        }
    }

    if (ServerExecuteIndex < 0)
    {
        Log::Error(L"Finder: UFortWorldItemDefinition::ServerExecute slot not found");
        return false;
    }
    Natives::UFortWorldItemDefinition_ServerExecuteIndex = ServerExecuteIndex;
    Log::Info(L"Finder: UFortWorldItemDefinition::ServerExecute is slot 0x{:X}", ServerExecuteIndex);
    return true;
}

bool Finder::ResolveAbilitySystemLive()
{
    MissingCount = 0;

    UFunction* ServerTryActivate = UAbilitySystemComponent::StaticFunction_ServerTryActivateAbility();
    const uintptr_t Exec = ServerTryActivate ? reinterpret_cast<uintptr_t>(ServerTryActivate->GetNativeFunc()) : 0;
    const uintptr_t ExecEnd = IsInText(Exec) ? Finder::FunctionEnd(Exec) : 0;
    int32_t ImplementationIndex = -1;
    for (uintptr_t Site = ExecEnd ? FindVirtualCall(Exec, ExecEnd) : 0; Site; Site = FindVirtualCall(Site + 1, ExecEnd))
    {
        ImplementationIndex = VirtualSlot(Site);
    }

    const uintptr_t Implementation = VTableEntry(CdoVTable(L"/Script/GameplayAbilities.AbilitySystemComponent"), ImplementationIndex);
    const int32_t ActivateIndex = Implementation ? VirtualSlot(FindVirtualCall(Implementation, Implementation + 0x20)) : -1;
    RequireSlot(L"UAbilitySystemComponent::InternalServerTryActiveAbility", Natives::UAbilitySystemComponent_InternalServerTryActiveAbilityIndex, ActivateIndex);

    const auto TryActivate = reinterpret_cast<uintptr_t>(Natives::UAbilitySystemComponent_InternalTryActivateAbility);
    const uintptr_t TryActivateEnd = IsInText(TryActivate) ? Finder::FunctionEnd(TryActivate) : 0;
    for (uintptr_t SetKey = TryActivateEnd ? FindInstruction(TryActivate, TryActivateEnd, { 0x41, 0xB1, 0x01 }) : 0; SetKey && !Natives::FScopedPredictionWindow_Destructor; SetKey = FindInstruction(SetKey + 1, TryActivateEnd, { 0x41, 0xB1, 0x01 }))
    {
        const uintptr_t Call = FindRawCall(SetKey, std::min(SetKey + 0x30, TryActivateEnd));
        const uintptr_t WindowLoad = Call ? FindInstruction(SetKey, Call, { 0x48, 0x8D, 0x4D, -1 }) : 0;
        const int32_t WindowOffset = WindowLoad ? Byte(WindowLoad + 3) : -1;
        const uintptr_t Destroy = WindowLoad ? FindInstruction(Call + 5, TryActivateEnd, { 0x48, 0x8D, 0x4D, WindowOffset, 0xE8 }) : 0;
        if (Destroy)
        {
            Natives::FScopedPredictionWindow_FScopedPredictionWindow = reinterpret_cast<decltype(Natives::FScopedPredictionWindow_FScopedPredictionWindow)>(Finder::RelativeTarget(Call, 1, 5));
            Natives::FScopedPredictionWindow_Destructor = reinterpret_cast<decltype(Natives::FScopedPredictionWindow_Destructor)>(Finder::RelativeTarget(Destroy + 4, 1, 5));
        }
    }
    if (!Natives::FScopedPredictionWindow_Destructor)
    {
        Log::Error(L"Finder: FScopedPredictionWindow not found in UAbilitySystemComponent::InternalTryActivateAbility");
    }

    Log::Info(L"Finder: ability system live slots resolved, {} missing", MissingCount);
    return MissingCount == 0;
}

bool Finder::ResolveGameModeZoneLive()
{
    MissingCount = 0;

    void** FortVTable = CdoVTable(L"/Script/FortniteGame.FortGameMode");
    const int32_t FinishIndex = Finder::VTableIndex(FortVTable, reinterpret_cast<void*>(Natives::AFortGameMode_FinishWorldInitialization), 0x400);
    RequireSlot(L"AFortGameMode::FinishWorldInitialization", Natives::AFortGameMode_FinishWorldInitializationIndex, FinishIndex);


    Log::Info(L"Finder: zone game mode live slots resolved, {} missing", MissingCount);
    return MissingCount == 0;
}

bool Finder::ResolveNavigationSystemLive()
{
    UFunction* RebuildNavigation = FindObject<UFunction>(nullptr, L"/Script/Engine.CheatManager:RebuildNavigation");
    const uintptr_t Exec = RebuildNavigation ? reinterpret_cast<uintptr_t>(RebuildNavigation->GetNativeFunc()) : 0;
    const uintptr_t ExecEnd = IsInText(Exec) ? (Finder::FunctionStart(Exec) == Exec ? Finder::FunctionEnd(Exec) : Exec + 0x40) : 0;
    const int32_t CheatSlot = ExecEnd ? VirtualSlot(FindVirtualCall(Exec, ExecEnd)) : -1;
    const uintptr_t CheatRebuild = VTableEntry(CdoVTable(L"/Script/Engine.CheatManager"), CheatSlot);
    const uintptr_t CheatRebuildEnd = CheatRebuild ? Finder::FunctionEnd(CheatRebuild) : 0;

    int32_t BuildSlot = -1;
    for (uintptr_t Site = CheatRebuildEnd ? FindVirtualCall(CheatRebuild, CheatRebuildEnd) : 0; Site; Site = FindVirtualCall(Site + 1, CheatRebuildEnd))
    {
        BuildSlot = VirtualSlot(Site);
    }
    const uintptr_t Build = VTableEntry(CdoVTable(L"/Script/Engine.NavigationSystem"), BuildSlot);
    const uintptr_t BuildEnd = Build ? Finder::FunctionEnd(Build) : 0;

    uintptr_t LockTest = 0;
    for (uintptr_t Site = Build; BuildEnd && Site + 7 <= BuildEnd && !LockTest; ++Site)
    {
        const uint8_t ModRm = Byte(Site + 1);
        if (Byte(Site) == 0xF6 && (ModRm & 0xF8) == 0x80 && (ModRm & 0x07) != 0x04 && Byte(Site + 6) == static_cast<uint8_t>(~ENavigationBuildLock::NoUpdateInEditor))
        {
            LockTest = Site;
        }
    }
    const ptrdiff_t LockFlagsOffset = LockTest ? *reinterpret_cast<const int32_t*>(LockTest + 2) : -1;

    const uintptr_t SpawnMissing = LockTest ? FindVirtualCall(LockTest, BuildEnd) : 0;
    const uintptr_t ProcessCandidates = SpawnMissing ? FindRawCall(SpawnMissing, BuildEnd) : 0;
    const uintptr_t RebuildAllCall = ProcessCandidates ? FindRawCall(ProcessCandidates + 5, BuildEnd) : 0;
    const uintptr_t RebuildAll = RebuildAllCall ? Finder::RelativeTarget(RebuildAllCall, 1, 5) : 0;

    const int32_t SpawnMissingSlot = VirtualSlot(SpawnMissing);
    const uintptr_t ProcessCandidatesTarget = ProcessCandidates ? Finder::RelativeTarget(ProcessCandidates, 1, 5) : 0;
    uintptr_t PopulateNavOctree = 0;
    int32_t PopulateCandidates = 0;
    for (uintptr_t Call = ProcessCandidatesTarget ? FindRawCall(Finder::TextBegin(), Finder::TextEnd()) : 0; Call; Call = FindRawCall(Call + 1, Finder::TextEnd()))
    {
        if (Finder::RelativeTarget(Call, 1, 5) != ProcessCandidatesTarget || Call == ProcessCandidates || VirtualSlot(FindVirtualCall(Call - 12, Call)) != SpawnMissingSlot)
        {
            continue;
        }
        const uintptr_t Next = FindRawCall(Call + 5, Call + 17);
        const uintptr_t Target = Next ? Finder::RelativeTarget(Next, 1, 5) : 0;
        if (Target && Target != RebuildAll && Target != PopulateNavOctree)
        {
            PopulateNavOctree = Target;
            ++PopulateCandidates;
        }
    }
    const uintptr_t PopulateEnd = PopulateCandidates == 1 ? Finder::FunctionEnd(PopulateNavOctree) : 0;
    ptrdiff_t OctreeLockOffset = -1;
    for (uintptr_t Site = PopulateNavOctree; PopulateEnd && Site + 7 <= PopulateEnd && OctreeLockOffset < 0; ++Site)
    {
        const uint8_t ModRm = Byte(Site + 1);
        if (Byte(Site) == 0xF6 && (ModRm & 0xF8) == 0x80 && (ModRm & 0x07) != 0x04 && Byte(Site + 6) == 0x01)
        {
            OctreeLockOffset = *reinterpret_cast<const int32_t*>(Site + 2);
        }
    }

    if (LockFlagsOffset <= 0 || !IsInText(RebuildAll) || OctreeLockOffset <= 0)
    {
        Log::Error(L"Finder: UNavigationSystem not resolved (cheat slot {}, build slot {}, lock flags {}, rebuild all {}, populate candidates {}, octree lock {})", CheatSlot, BuildSlot, LockFlagsOffset, RebuildAll != 0, PopulateCandidates, OctreeLockOffset);
        return false;
    }
    Natives::UNavigationSystem_NavBuildingLockFlags = LockFlagsOffset;
    Natives::UNavigationSystem_RebuildAll = reinterpret_cast<decltype(Natives::UNavigationSystem_RebuildAll)>(RebuildAll);
    Natives::UNavigationSystem_ConditionalPopulateNavOctree = reinterpret_cast<decltype(Natives::UNavigationSystem_ConditionalPopulateNavOctree)>(PopulateNavOctree);
    Natives::UNavigationSystem_bNavOctreeLock = OctreeLockOffset;
    Log::Info(L"Finder: UNavigationSystem::Build is slot 0x{:X}, NavBuildingLockFlags +0x{:X}, RebuildAll +0x{:X}, ConditionalPopulateNavOctree +0x{:X}, bNavOctreeLock +0x{:X}", BuildSlot, LockFlagsOffset, RebuildAll - Finder::ImageBase(), PopulateNavOctree - Finder::ImageBase(), OctreeLockOffset);
    return true;
}

bool Finder::ResolveOutpostOwnerLive()
{
    UFunction* IsOutpostOwner = FindObject<UFunction>(nullptr, L"/Script/FortniteGame.FortPlayerStateOutpost:IsOutpostOwner");
    const uintptr_t Exec = IsOutpostOwner ? reinterpret_cast<uintptr_t>(IsOutpostOwner->GetNativeFunc()) : 0;
    const uintptr_t ExecEnd = IsInText(Exec) ? (Finder::FunctionStart(Exec) == Exec ? Finder::FunctionEnd(Exec) : Exec + 0x40) : 0;
    const uintptr_t Call = ExecEnd ? FindRawCall(Exec, ExecEnd) : 0;
    const uintptr_t Implementation = Call ? Finder::RelativeTarget(Call, 1, 5) : 0;
    const uintptr_t Load = IsInText(Implementation) ? FindInstruction(Implementation, Implementation + 0x10, { 0x8B, 0x81, -1, -1, -1, -1, 0x83, 0xE0, -1 }) : 0;
    if (!Load)
    {
        Log::Error(L"Finder: AFortPlayerStateOutpost::IsOutpostOwner not decoded");
        return false;
    }
    Natives::AFortPlayerStateOutpost_bIsOutpostOwner = *reinterpret_cast<const int32_t*>(Load + 2);
    Natives::AFortPlayerStateOutpost_bIsOutpostOwnerMask = Byte(Load + 8);
    Log::Info(L"Finder: AFortPlayerStateOutpost owner bit +0x{:X} mask 0x{:X}", Natives::AFortPlayerStateOutpost_bIsOutpostOwner, Natives::AFortPlayerStateOutpost_bIsOutpostOwnerMask);
    return true;
}

bool Finder::ResolveBadgeScoresLive()
{
    if (Natives::AFortGameMode_SetBadgeScores)
    {
        return true;
    }

    uintptr_t Function = 0;
    bool bAmbiguous = false;
    for (uintptr_t Reference : Finder::StringRefs(L"Badges score was %d"))
    {
        const uintptr_t Start = Finder::FunctionStart(Reference);
        bAmbiguous |= Function && Start != Function;
        Function = Start;
    }
    if (!Function || bAmbiguous)
    {
        Log::Error(L"Finder: AFortGameMode::SetBadgeScores not found");
        return false;
    }
    Natives::AFortGameMode_SetBadgeScores = reinterpret_cast<decltype(Natives::AFortGameMode_SetBadgeScores)>(Function);
    Log::Info(L"Finder: AFortGameMode::SetBadgeScores found");
    return true;
}

bool Finder::ResolveEarnScoreLive(ptrdiff_t CriticalMatchBonusOffset, FEarnScoreCallSites& OutSites)
{
    const auto IsStub = [](uintptr_t Target, uint8_t Opcode)
    {
        return Byte(Target) == Opcode && Byte(Target + 1) == 0xC0 && Byte(Target + 2) == 0xC3;
    };

    // AFortGameMode::SendEarnScore: the first AFortGameMode slot whose body reads the total score stat (lea edx, [r8+1Bh],
    // GetStat 27) and calls one 32 C0 C3 stub (the earn gate) and one 33 C0 C3 stub (the badges score). 0x135 on 1.1 and
    // 1.2, 0x139 on 1.7.2, which has no "Sending EarnScore command via account" log line to anchor on.
    void** GameModeVTable = CdoVTable(L"/Script/FortniteGame.FortGameMode");
    const int32_t Index = FirstVTableIndexWhere(GameModeVTable, [&IsStub](uintptr_t Entry)
    {
        const uintptr_t EntryEnd = Finder::FunctionStart(Entry) == Entry ? Finder::FunctionEnd(Entry) : 0;
        if (EntryEnd <= Entry || EntryEnd - Entry > 0x2000 || !FindInstruction(Entry, EntryEnd, { 0x41, 0x8D, 0x50, 0x1B }))
        {
            return false;
        }
        int32_t Gates = 0;
        int32_t Scores = 0;
        for (const auto& [Call, Target] : Finder::DirectCalls(Entry, EntryEnd))
        {
            Gates += IsStub(Target, 0x32) ? 1 : 0;
            Scores += IsStub(Target, 0x33) ? 1 : 0;
        }
        return Gates == 1 && Scores == 1;
    });
    const uintptr_t Function = VTableEntry(GameModeVTable, Index);
    const uintptr_t End = Function ? Finder::FunctionEnd(Function) : 0;
    uintptr_t GateCall = 0;
    uintptr_t ScoreCall = 0;
    int32_t GateCount = 0;
    int32_t ScoreCount = 0;
    const auto Calls = End ? Finder::DirectCalls(Function, End) : decltype(Finder::DirectCalls(0, 0)){};
    for (const auto& [Call, Target] : Calls)
    {
        if (IsStub(Target, 0x32))
        {
            GateCall = Call;
            ++GateCount;
        }
        if (IsStub(Target, 0x33))
        {
            ScoreCall = Call;
            ++ScoreCount;
        }
    }

    const int32_t Critical = static_cast<int32_t>(CriticalMatchBonusOffset);
    uintptr_t CriticalLoad = 0;
    for (uintptr_t Address = ScoreCall; Address && !CriticalLoad && Address + 7 <= End; ++Address)
    {
        const bool bDisp32 = (Byte(Address + 2) & 0xC0) == 0x80 && (Byte(Address + 2) & 0x07) != 0x04;
        if (Byte(Address) == 0x0F && Byte(Address + 1) == 0xB6 && bDisp32 && *reinterpret_cast<const int32_t*>(Address + 3) == Critical)
        {
            CriticalLoad = Address;
        }
    }

    // The EarnScore context maker: a small function that allocates the shared-ref controller (mov ecx, 20h; call). It is
    // the first such call after the critical match bonus load in SendEarnScore (1.1, 1.2), or in the callee 1.7.2 moved
    // it to (GrantScoreReward 0x57B4A0, which builds the context only when bIsFinal is set).
    const auto IsContextMaker = [](uintptr_t Target)
    {
        const uintptr_t TargetEnd = Finder::FunctionStart(Target) == Target ? Finder::FunctionEnd(Target) : 0;
        return TargetEnd > Target && TargetEnd - Target < 0x80 && FindInstruction(Target, TargetEnd, { 0xB9, 0x20, 0x00, 0x00, 0x00, 0xE8 }) != 0;
    };
    uintptr_t GrowCall = 0;
    uintptr_t GrowTarget = 0;
    uintptr_t ContextCall = 0;
    uintptr_t ContextTarget = 0;
    for (const auto& [Call, Target] : Calls)
    {
        if (CriticalLoad && Call > ScoreCall && Call < CriticalLoad)
        {
            GrowCall = Call;
            GrowTarget = Target;
        }
        if (CriticalLoad && !ContextCall && Call > CriticalLoad && IsContextMaker(Target))
        {
            ContextCall = Call;
            ContextTarget = Target;
        }
    }
    for (const auto& [Call, Target] : Calls)
    {
        if (!CriticalLoad || ContextCall || Call < CriticalLoad || Finder::FunctionStart(Target) != Target)
        {
            continue;
        }
        const uintptr_t TargetEnd = Finder::FunctionEnd(Target);
        for (const auto& [InnerCall, InnerTarget] : TargetEnd > Target ? Finder::DirectCalls(Target, TargetEnd) : std::vector<std::pair<uintptr_t, uintptr_t>>{})
        {
            if (IsContextMaker(InnerTarget))
            {
                ContextCall = InnerCall;
                ContextTarget = InnerTarget;
                break;
            }
        }
    }

    const uintptr_t TotalScoreLoad = End ? FindInstruction(Function, End, { 0x41, 0x8D, 0x50, 0x1B }) : 0;
    const int32_t GetStatIndex = TotalScoreLoad ? VirtualSlot(FindVirtualCall(TotalScoreLoad + 4, TotalScoreLoad + 0x10)) : -1;

    if (Index < 0 || GateCount != 1 || ScoreCount != 1 || !GrowCall || !ContextCall || GetStatIndex < 0)
    {
        Log::Error(L"Finder: AFortGameMode EarnScore sender not decoded (slot {}, gate calls {}, score calls {}, grow {}, context {}, GetStat slot {})", Index, GateCount, ScoreCount, GrowCall != 0, ContextCall != 0, GetStatIndex);
        return false;
    }

    OutSites.GateCall = reinterpret_cast<uint8_t*>(GateCall);
    OutSites.ScoreCall = reinterpret_cast<uint8_t*>(ScoreCall);
    OutSites.GrowCall = reinterpret_cast<uint8_t*>(GrowCall);
    OutSites.ContextCall = reinterpret_cast<uint8_t*>(ContextCall);
    Natives::AFortGameMode_SendEarnScoreIndex = Index;
    Natives::AFortGameMode_MakeEarnScoreContext = reinterpret_cast<decltype(Natives::AFortGameMode_MakeEarnScoreContext)>(ContextTarget);
    Natives::TArray_FAccountIdAndScore_ResizeGrow = reinterpret_cast<decltype(Natives::TArray_FAccountIdAndScore_ResizeGrow)>(GrowTarget);
    Natives::AFortPlayerState_GetStatIndex = GetStatIndex;
    Log::Info(L"Finder: AFortGameMode EarnScore sender is slot 0x{:X}, AFortPlayerState::GetStat slot 0x{:X}", Index, GetStatIndex);
    return true;
}

uint8_t* Finder::SharedEmptyCallInImplementation(UFunction* Function)
{
    const uintptr_t Exec = Function ? reinterpret_cast<uintptr_t>(Function->GetNativeFunc()) : 0;
    const uintptr_t ExecEnd = IsInText(Exec) ? Finder::FunctionEnd(Exec) : 0;
    const auto SharedEmpty = reinterpret_cast<uintptr_t>(Natives::SharedEmptyFunction);
    uintptr_t Found = 0;
    int32_t Count = 0;
    for (uintptr_t Call = ExecEnd && SharedEmpty ? FindRawCall(Exec, ExecEnd) : 0; Call; Call = FindRawCall(Call + 1, ExecEnd))
    {
        const uintptr_t Implementation = Finder::RelativeTarget(Call, 1, 5);
        const uintptr_t ImplementationEnd = Finder::FunctionStart(Implementation) == Implementation ? Finder::FunctionEnd(Implementation) : 0;
        for (uintptr_t Inner = ImplementationEnd ? FindRawCall(Implementation, ImplementationEnd) : 0; Inner; Inner = FindRawCall(Inner + 1, ImplementationEnd))
        {
            if (Finder::RelativeTarget(Inner, 1, 5) == SharedEmpty)
            {
                Found = Inner;
                ++Count;
            }
        }
    }
    return Count == 1 ? reinterpret_cast<uint8_t*>(Found) : nullptr;
}

bool Finder::ResolveInventoryLive()
{
    if (Natives::AFortInventory_InitializeExistingItem)
    {
        return true;
    }

    UFunction* LocalUpdate = FindObject<UFunction>(nullptr, L"/Script/FortniteGame.FortInventory:HandleInventoryLocalUpdate");
    const uintptr_t Exec = LocalUpdate ? reinterpret_cast<uintptr_t>(LocalUpdate->GetNativeFunc()) : 0;
    uintptr_t Update = 0;
    for (uintptr_t Site = IsInText(Exec) ? Exec : 0; Site && Site < Exec + 0x40 && !Update; ++Site)
    {
        if ((Byte(Site) == 0xE8 || Byte(Site) == 0xE9) && IsInText(Finder::RelativeTarget(Site, 1, 5)))
        {
            Update = Finder::RelativeTarget(Site, 1, 5);
        }
    }
    const uintptr_t UpdateCall = Update ? FindRawCall(Update, Update + 0x20) : 0;
    const uintptr_t UpdateItemInstances = UpdateCall ? Finder::RelativeTarget(UpdateCall, 1, 5) : 0;
    uintptr_t InitializeExistingItem = 0;
    bool bAmbiguous = false;
    for (uintptr_t Call : UpdateItemInstances ? Finder::CallsTo(UpdateItemInstances, false) : std::vector<uintptr_t>{})
    {
        const uintptr_t Caller = Finder::FunctionStart(Call);
        if (Caller && Caller != Update)
        {
            bAmbiguous |= InitializeExistingItem && InitializeExistingItem != Caller;
            InitializeExistingItem = Caller;
        }
    }
    if (!InitializeExistingItem || bAmbiguous)
    {
        Log::Error(L"Finder: AFortInventory::InitializeExistingItem not found");
        return false;
    }

    Natives::AFortInventory_InitializeExistingItem = reinterpret_cast<decltype(Natives::AFortInventory_InitializeExistingItem)>(InitializeExistingItem);
    Log::Info(L"Finder: AFortInventory::InitializeExistingItem found");
    return true;
}

bool Finder::ResolveBackpackLive(ptrdiff_t PrimaryPickupItemEntryOffset)
{
    if (Natives::AFortInventory_CanStackWith)
    {
        return true;
    }

    const auto Targets = [](uintptr_t Function)
    {
        std::vector<uintptr_t> Result;
        const uintptr_t FunctionEnd = IsInText(Function) ? Finder::FunctionEnd(Function) : 0;
        for (const auto& [Call, Target] : FunctionEnd > Function ? Finder::DirectCalls(Function, FunctionEnd) : std::vector<std::pair<uintptr_t, uintptr_t>>{})
        {
            Result.push_back(Target);
        }
        return Result;
    };

    const uintptr_t ErrorText = Finder::FunctionStart(OnlyRef(L"Backpack full!"));
    const uintptr_t ErrorTextEnd = ErrorText ? Finder::FunctionEnd(ErrorText) : 0;
    uintptr_t OverflowReason = 0;
    for (uintptr_t Site = ErrorTextEnd ? FindInstruction(ErrorText, ErrorTextEnd, { 0x48, 0x8D, -1 }) : 0; Site && !OverflowReason; Site = FindInstruction(Site + 1, ErrorTextEnd, { 0x48, 0x8D, -1 }))
    {
        if ((Byte(Site + 2) & 0xF8) == 0x90 && Byte(Site + 2) != 0x94 && *reinterpret_cast<const int32_t*>(Site + 3) == PrimaryPickupItemEntryOffset)
        {
            const uintptr_t Call = FindRawCall(Site + 7, std::min(Site + 0x20, ErrorTextEnd));
            OverflowReason = Call ? Finder::RelativeTarget(Call, 1, 5) : 0;
        }
    }

    const std::vector<uintptr_t> ReasonCalls = Targets(OverflowReason);
    const uintptr_t StackLimit = ReasonCalls.size() >= 2 ? ReasonCalls[ReasonCalls.size() - 2] : 0;
    const uintptr_t BackpackLimit = ReasonCalls.size() >= 2 ? ReasonCalls.back() : 0;
    const uintptr_t StackLimitEnd = IsInText(StackLimit) ? Finder::FunctionEnd(StackLimit) : 0;
    const uintptr_t BackpackLimitEnd = IsInText(BackpackLimit) ? Finder::FunctionEnd(BackpackLimit) : 0;
    const bool bStackCap = BackpackLimitEnd > BackpackLimit && FindInstruction(BackpackLimit, BackpackLimitEnd, { 0x3D, 0x00, 0x02, 0x00, 0x00 });

    const std::vector<uintptr_t> StackLimitCalls = Targets(StackLimit);
    uintptr_t MaxStackSize = 0;
    for (const uintptr_t Target : Targets(BackpackLimit))
    {
        // 1.7.2's limits both also call a leaf getter (0x219A8F0, no .pdata entry) after GetMaxStackSize.
        if (Finder::FunctionStart(Target) == Target && std::find(StackLimitCalls.begin(), StackLimitCalls.end(), Target) != StackLimitCalls.end())
        {
            MaxStackSize = Target;
        }
    }

    uintptr_t CanStackWith = 0;
    bool bAmbiguous = false;
    for (const auto& [Call, Target] : StackLimitEnd > StackLimit ? Finder::DirectCalls(StackLimit, StackLimitEnd) : std::vector<std::pair<uintptr_t, uintptr_t>>{})
    {
        if (Call + 7 <= StackLimitEnd && Byte(Call + 5) == 0x84 && Byte(Call + 6) == 0xC0)
        {
            bAmbiguous |= CanStackWith && CanStackWith != Target;
            CanStackWith = Target;
        }
    }

    ptrdiff_t InterfaceOffset = -1;
    int32_t NumItemsSlot = -1;
    int32_t MaxItemsSlot = -1;
    for (uintptr_t Site = BackpackLimit; Site && Site + 3 <= BackpackLimitEnd && MaxItemsSlot < 0; ++Site)
    {
        if ((Byte(Site) & 0xF8) == 0x48 && Byte(Site + 1) == 0x8D && (Byte(Site + 2) & 0xC0) == 0x80 && (Byte(Site + 2) & 0x07) != 0x04 && InterfaceOffset < 0)
        {
            const int32_t Displacement = *reinterpret_cast<const int32_t*>(Site + 3);
            InterfaceOffset = Displacement > 0 && Displacement < 0x2000 ? Displacement : -1;
        }
        else if (InterfaceOffset > 0 && Byte(Site) == 0xFF && (Byte(Site + 1) & 0xF8) == 0x50 && Byte(Site + 1) != 0x54 && Byte(Site + 2) % 8 == 0)
        {
            const int32_t Slot = Byte(Site + 2) / 8;
            if (NumItemsSlot < 0)
            {
                NumItemsSlot = Slot;
            }
            else if (Slot != NumItemsSlot)
            {
                MaxItemsSlot = Slot;
            }
        }
    }

    if (!OverflowReason || !StackLimit || !bStackCap || !MaxStackSize || !CanStackWith || bAmbiguous || MaxItemsSlot < 0)
    {
        Log::Error(L"Finder: AFortInventory overflow checks not found (reason {}, stack cap {}, max stack {}, can stack {}, slots {}/{})", OverflowReason != 0, bStackCap, MaxStackSize != 0, CanStackWith != 0, NumItemsSlot, MaxItemsSlot);
        return false;
    }

    Natives::AFortInventory_GetItemStackLimitOverflow = reinterpret_cast<decltype(Natives::AFortInventory_GetItemStackLimitOverflow)>(StackLimit);
    Natives::AFortInventory_GetBackpackStackLimitOverflow = reinterpret_cast<decltype(Natives::AFortInventory_GetBackpackStackLimitOverflow)>(BackpackLimit);
    Natives::AFortInventory_GetMaxStackSize = reinterpret_cast<decltype(Natives::AFortInventory_GetMaxStackSize)>(MaxStackSize);
    Natives::AFortInventory_InventoryInterfaceOffset = InterfaceOffset;
    Natives::IFortInventoryInterface_GetNumItemsIndex = NumItemsSlot;
    Natives::IFortInventoryInterface_GetMaxItemsIndex = MaxItemsSlot;
    Natives::AFortInventory_CanStackWith = reinterpret_cast<decltype(Natives::AFortInventory_CanStackWith)>(CanStackWith);
    Log::Info(L"Finder: AFortInventory overflow checks found (stack limit +0x{:X}, backpack limit +0x{:X}, interface +0x{:X} slots {}/{})", StackLimit - Finder::ImageBase(), BackpackLimit - Finder::ImageBase(), InterfaceOffset, NumItemsSlot, MaxItemsSlot);
    return true;
}

bool Finder::ResolveWorldItemCountLive(ptrdiff_t RegenCooldownOffset, ptrdiff_t ItemEntryOffset)
{
    if (Natives::UFortWorldItem_SetItemCountIndex >= 0)
    {
        return true;
    }

    const auto FindDisplacement = [](uintptr_t Begin, uintptr_t End, std::initializer_list<int32_t> Opcode, ptrdiff_t Displacement)
    {
        for (uintptr_t Site = FindInstruction(Begin, End, Opcode); Site; Site = FindInstruction(Site + 1, End, Opcode))
        {
            if (Site + Opcode.size() + sizeof(int32_t) <= End && *reinterpret_cast<const int32_t*>(Site + Opcode.size()) == Displacement)
            {
                return Site;
            }
        }
        return uintptr_t{ 0 };
    };

    void** AmmoVTable = CdoVTable(L"/Script/FortniteGame.FortAmmoItemDefinition");
    uintptr_t RegenCompare = 0;
    uintptr_t RegenFunctionEnd = 0;
    bool bAmbiguous = false;
    for (int32_t Index = 0; AmmoVTable && RegenCooldownOffset > 0 && Index < 0x100; ++Index)
    {
        const uintptr_t Function = VTableEntry(AmmoVTable, Index);
        const uintptr_t FunctionEnd = Function ? Finder::FunctionEnd(Function) : 0;
        const uintptr_t Compare = FunctionEnd > Function ? FindDisplacement(Function, FunctionEnd, { 0x0F, 0x2F, 0x82 }, RegenCooldownOffset) : 0;
        if (Compare)
        {
            bAmbiguous |= RegenCompare && RegenCompare != Compare;
            RegenCompare = Compare;
            RegenFunctionEnd = FunctionEnd;
        }
    }

    const uintptr_t CountCall = RegenCompare ? FindInstruction(RegenCompare, RegenFunctionEnd, { 0xFF, 0x93 }) : 0;
    const int32_t Displacement = CountCall ? *reinterpret_cast<const int32_t*>(CountCall + 2) : 0;
    const int32_t SetItemCountIndex = Displacement > 0 && Displacement % 8 == 0 ? Displacement / 8 : -1;
    const uintptr_t SetItemCount = VTableEntry(CdoVTable(L"/Script/FortniteGame.FortWorldItem"), SetItemCountIndex);
    const uintptr_t SetItemCountEnd = SetItemCount ? Finder::FunctionEnd(SetItemCount) : 0;
    const bool bSetItemCount = SetItemCountEnd > SetItemCount
        && FindDisplacement(SetItemCount, SetItemCountEnd, { 0x48, 0x8D, 0xBB }, ItemEntryOffset)
        && FindInstruction(SetItemCount, SetItemCountEnd, { 0x80, 0x38, 0x03 });
    if (bAmbiguous || !bSetItemCount)
    {
        Log::Error(L"Finder: UFortWorldItem::SetItemCount slot not found");
        return false;
    }

    Natives::UFortWorldItem_SetItemCountIndex = SetItemCountIndex;
    Log::Info(L"Finder: UFortWorldItem::SetItemCount is slot 0x{:X}", SetItemCountIndex);
    return true;
}

bool Finder::ResolveItemEntryStateLive()
{
    if (Natives::FFortItemEntry_SetStateValue && Natives::FFortItemEntry_MarkDirty)
    {
        return true;
    }

    UFunction* NotifyFabrication = FindObject<UFunction>(nullptr, L"/Script/FortniteGame.FortPlayerControllerOutpost:ServerNotifyPendingFabricationCompleted");
    const uintptr_t Implementation = VTableEntry(CdoVTable(L"/Script/FortniteGame.FortPlayerControllerOutpost"), ImplementationIndex(NotifyFabrication));
    const uintptr_t ImplementationEnd = Implementation ? Finder::FunctionEnd(Implementation) : 0;
    std::vector<uintptr_t> Targets;
    for (const auto& [Call, Target] : ImplementationEnd > Implementation ? Finder::DirectCalls(Implementation, ImplementationEnd) : std::vector<std::pair<uintptr_t, uintptr_t>>{})
    {
        Targets.push_back(Target);
    }
    const uintptr_t SetStateValue = Targets.size() == 4 && Targets[0] == Targets[2] && Targets[1] == Targets[3] && Targets[0] != Targets[1] ? Targets[1] : 0;

    const uintptr_t SetStateValueEnd = SetStateValue ? Finder::FunctionEnd(SetStateValue) : 0;
    uintptr_t MarkDirty = 0;
    bool bAmbiguous = false;
    for (uintptr_t Site = SetStateValue; Site && Site + 5 <= SetStateValueEnd; ++Site)
    {
        const uintptr_t Target = Byte(Site) == 0xE9 ? Finder::RelativeTarget(Site, 1, 5) : 0;
        if (IsInText(Target) && Finder::FunctionStart(Target) == Target)
        {
            bAmbiguous |= MarkDirty && MarkDirty != Target;
            MarkDirty = Target;
        }
    }
    if (!SetStateValue || !MarkDirty || bAmbiguous)
    {
        Log::Error(L"Finder: FFortItemEntry::SetStateValue not found (calls {}, mark dirty {})", Targets.size(), MarkDirty != 0);
        return false;
    }

    Natives::FFortItemEntry_SetStateValue = reinterpret_cast<decltype(Natives::FFortItemEntry_SetStateValue)>(SetStateValue);
    Natives::FFortItemEntry_MarkDirty = reinterpret_cast<decltype(Natives::FFortItemEntry_MarkDirty)>(MarkDirty);
    Log::Info(L"Finder: FFortItemEntry::SetStateValue and MarkDirty found");
    return true;
}

bool Finder::ResolveNeverPersistItemDefinitionLive()
{
    if (Natives::UFortNeverPersistItemDefinition_OnItemCountAddedIndex >= 0)
    {
        return true;
    }

    // UFortWorldItemDefinition::OnItemCountAdded and OnItemCountRemoved: adjacent small slots that broadcast the item
    // count delegate through the same function, with Count and -Count (neg r8d only in Removed). 1.1 and 1.2's
    // UFortNeverPersistItemDefinition overrides both with stripped jmp-to-Super thunks; 1.7.2's doesn't override them.
    // The first pair on the world item definition's vtable is the one (subclass vtables after it repeat it).
    void** WorldItemDefinitionVTable = CdoVTable(L"/Script/FortniteGame.FortWorldItemDefinition");
    const auto SingleCallTarget = [](uintptr_t Entry)
    {
        const uintptr_t EntryEnd = Entry && Finder::FunctionStart(Entry) == Entry ? Finder::FunctionEnd(Entry) : 0;
        const auto Calls = EntryEnd > Entry && EntryEnd - Entry < 0x40 ? Finder::DirectCalls(Entry, EntryEnd) : std::vector<std::pair<uintptr_t, uintptr_t>>{};
        return Calls.size() == 1 ? Calls[0].second : 0;
    };
    const auto NegatesCount = [](uintptr_t Entry)
    {
        return FindInstruction(Entry, Finder::FunctionEnd(Entry), { 0x41, 0xF7, 0xD8 }) != 0;
    };
    int32_t AddedIndex = -1;
    for (int32_t Index = 1; WorldItemDefinitionVTable && Index < 0x100 && AddedIndex < 0; ++Index)
    {
        const uintptr_t Added = VTableEntry(WorldItemDefinitionVTable, Index);
        const uintptr_t Removed = VTableEntry(WorldItemDefinitionVTable, Index + 1);
        const uintptr_t Target = SingleCallTarget(Added);
        if (Target && SingleCallTarget(Removed) == Target && !NegatesCount(Added) && NegatesCount(Removed))
        {
            AddedIndex = Index;
        }
    }
    if (AddedIndex < 0)
    {
        Log::Error(L"Finder: UFortNeverPersistItemDefinition item count slots not found");
        return false;
    }

    Natives::UFortNeverPersistItemDefinition_OnItemCountAddedIndex = AddedIndex;
    Natives::UFortNeverPersistItemDefinition_OnItemCountRemovedIndex = AddedIndex + 1;
    Log::Info(L"Finder: UFortNeverPersistItemDefinition::OnItemCountAdded is slot 0x{:X}, OnItemCountRemoved slot 0x{:X}", AddedIndex, AddedIndex + 1);
    return true;
}

bool Finder::ResolveDisassemblyLive(UFunction* ServerDisassembleInventoryItem, UObject* PlayerControllerDefault)
{
    if (Natives::AFortPlayerController_DisassembleInventoryItem && Natives::UFortWorldItemDefinition_GetDisassemblyResults)
    {
        return true;
    }

    const auto FirstCall = [](uintptr_t Function)
    {
        const uintptr_t FunctionEnd = IsInText(Function) ? Finder::FunctionEnd(Function) : 0;
        const uintptr_t Call = FunctionEnd ? FindRawCall(Function, FunctionEnd) : 0;
        return Call ? Finder::RelativeTarget(Call, 1, 5) : 0;
    };

    UFunction* CanBeDisassembledFunction = FindObject<UFunction>(nullptr, L"/Script/FortniteGame.FortWorldItemDefinition:CanBeDisassembled");
    const uintptr_t CanBeDisassembled = FirstCall(CanBeDisassembledFunction ? reinterpret_cast<uintptr_t>(CanBeDisassembledFunction->GetNativeFunc()) : 0);

    void** PlayerControllerVTable = PlayerControllerDefault ? *reinterpret_cast<void***>(PlayerControllerDefault) : nullptr;
    const uintptr_t Disassemble = FirstCall(VTableEntry(PlayerControllerVTable, ImplementationIndex(ServerDisassembleInventoryItem)));
    const uintptr_t DisassembleEnd = Disassemble ? Finder::FunctionEnd(Disassemble) : 0;
    const uintptr_t Results = FunctionOf(L"UFortWorldItemDefinition::GetDisassemblyResults");

    if (!CanBeDisassembled || !DisassembleEnd || !CallWithin(Disassemble, DisassembleEnd - Disassemble, CanBeDisassembled) || FirstCall(Results) != CanBeDisassembled)
    {
        Log::Error(L"Finder: item disassembly not found");
        return false;
    }

    Natives::AFortPlayerController_DisassembleInventoryItem = reinterpret_cast<decltype(Natives::AFortPlayerController_DisassembleInventoryItem)>(Disassemble);
    Natives::UFortWorldItemDefinition_GetDisassemblyResults = reinterpret_cast<decltype(Natives::UFortWorldItemDefinition_GetDisassemblyResults)>(Results);
    Log::Info(L"Finder: AFortPlayerController::DisassembleInventoryItem +0x{:X}, UFortWorldItemDefinition::GetDisassemblyResults +0x{:X}", Disassemble - Finder::ImageBase(), Results - Finder::ImageBase());
    return true;
}

bool Finder::ResolveLeaveOutpostLive()
{
    if (Natives::AFortGameMode_SendStayWithParty && Natives::AGameMode_ReturnToMainMenuHostIndex >= 0)
    {
        return true;
    }

    const int32_t ReturnIndex = Finder::VTableIndex(CdoVTable(L"/Script/FortniteGame.FortGameMode"), reinterpret_cast<void*>(FunctionOf(L"Ignoring ReturnToMainMenuHost, already in a level travel")), 0x400);
    const uintptr_t OnZoneEnd = Natives::AFortGameModeZone_OnZoneEndIndex >= 0 ? VTableEntry(CdoVTable(L"/Script/FortniteGame.FortGameModeZone"), Natives::AFortGameModeZone_OnZoneEndIndex) : 0;
    // The ClientStayWithParty RPC stub: the AFortPlayerController slot whose small body loads the function's name global
    // (mov rdx, [rip+Name]), which its static FName initializer fills (lea rdx, L"ClientStayWithParty"; lea rcx, Name).
    // Slot 0x29F on 1.1 and 1.2, 0x2D0 on 1.7.2.
    const uintptr_t NameInit = First(Finder::StringRefs(L"ClientStayWithParty"));
    const uintptr_t NameStore = NameInit ? FindInstruction(NameInit + 7, NameInit + 0x20, { 0x48, 0x8D, 0x0D }) : 0;
    const uintptr_t NameGlobal = NameStore ? Finder::RelativeTarget(NameStore, 3, 7) : 0;
    const int32_t StaySlot = NameGlobal ? FirstVTableIndexWhere(CdoVTable(L"/Script/FortniteGame.FortPlayerController"), [NameGlobal](uintptr_t Entry)
    {
        const uintptr_t EntryEnd = Finder::FunctionStart(Entry) == Entry ? Finder::FunctionEnd(Entry) : 0;
        for (uintptr_t Load = EntryEnd > Entry && EntryEnd - Entry < 0x200 ? FindInstruction(Entry, EntryEnd, { 0x48, 0x8B, 0x15 }) : 0; Load; Load = FindInstruction(Load + 1, EntryEnd, { 0x48, 0x8B, 0x15 }))
        {
            if (Finder::RelativeTarget(Load, 3, 7) == NameGlobal)
            {
                return true;
            }
        }
        return false;
    }) : -1;
    const int32_t StayCall = StaySlot * 8;
    uintptr_t StayWithParty = 0;
    for (uintptr_t Call = OnZoneEnd && StaySlot >= 0 ? FindRawCall(OnZoneEnd, OnZoneEnd + 0x200) : 0; Call && !StayWithParty; Call = FindRawCall(Call + 1, OnZoneEnd + 0x200))
    {
        const uintptr_t Target = Finder::RelativeTarget(Call, 1, 5);
        const uintptr_t TargetEnd = Finder::FunctionStart(Target) == Target ? Finder::FunctionEnd(Target) : 0;
        StayWithParty = TargetEnd && FindInstruction(Target, TargetEnd, { 0xFF, 0x90, StayCall & 0xFF, (StayCall >> 8) & 0xFF, (StayCall >> 16) & 0xFF, 0x00 }) ? Target : 0;
    }
    if (ReturnIndex < 0 || !StayWithParty)
    {
        Log::Error(L"Finder: leave outpost not found (return to main menu host slot {}, stay with party {})", ReturnIndex, StayWithParty != 0);
        return false;
    }

    Natives::AGameMode_ReturnToMainMenuHostIndex = ReturnIndex;
    Natives::AFortGameMode_SendStayWithParty = reinterpret_cast<decltype(Natives::AFortGameMode_SendStayWithParty)>(StayWithParty);
    Log::Info(L"Finder: AGameMode::ReturnToMainMenuHost is slot 0x{:X}", ReturnIndex);
    return true;
}

bool Finder::ResolveResetGameplayAttributesLive()
{
    if (Natives::AFortPlayerPawn_ResetGameplayAttributesIndex >= 0)
    {
        return true;
    }

    const int32_t Index = Finder::VTableIndex(CdoVTable(L"/Script/FortniteGame.FortPlayerPawn"), reinterpret_cast<void*>(FunctionOf(L"ResetGameplayAttributes called with valid state")), 0x400);
    if (Index < 0)
    {
        Log::Error(L"Finder: AFortPlayerPawn::ResetGameplayAttributes slot not found");
        return false;
    }

    Natives::AFortPlayerPawn_ResetGameplayAttributesIndex = Index;
    Log::Info(L"Finder: AFortPlayerPawn::ResetGameplayAttributes is slot 0x{:X}", Index);
    return true;
}

uint8_t* Finder::QuickBarChangedWorldManagerCall()
{
    const uintptr_t SwapItems = FunctionOf(L"Bad call to AFortQuickBars::SwapItems with index %d %d!");
    const uintptr_t SwapItemsEnd = SwapItems ? Finder::FunctionEnd(SwapItems) : 0;
    std::vector<uintptr_t> Checked;
    uintptr_t Found = 0;
    int32_t Count = 0;
    for (const auto& [Call, Target] : SwapItemsEnd ? Finder::DirectCalls(SwapItems, SwapItemsEnd) : decltype(Finder::DirectCalls(0, 0)){})
    {
        if (std::find(Checked.begin(), Checked.end(), Target) != Checked.end() || Finder::FunctionStart(Target) != Target)
        {
            continue;
        }
        Checked.push_back(Target);
        for (const auto& [Inner, InnerTarget] : Finder::DirectCalls(Target, Finder::FunctionEnd(Target)))
        {
            if (Byte(InnerTarget) == 0xB0 && Byte(InnerTarget + 1) == 0x01 && Byte(InnerTarget + 2) == 0xC3)
            {
                Found = Inner;
                ++Count;
            }
        }
    }

    if (Count != 1)
    {
        Log::Error(L"Finder: AFortQuickBars change handler's world manager call not found ({} candidates)", Count);
        return nullptr;
    }
    Log::Info(L"Finder: AFortQuickBars change handler's world manager call is at 0x{:X}", Found - Finder::ImageBase());
    return reinterpret_cast<uint8_t*>(Found);
}

namespace
{
    bool ContainsDisp32(uintptr_t Begin, uintptr_t End, int32_t Displacement)
    {
        for (uintptr_t Address = Begin; Address + 4 <= End; ++Address)
        {
            if (*reinterpret_cast<const int32_t*>(Address) == Displacement)
            {
                return true;
            }
        }
        return false;
    }

    int32_t UniqueSlotReferencing(void** VTable, size_t MaxSize, std::initializer_list<ptrdiff_t> Offsets, bool bNeedsCall)
    {
        int32_t Found = -1;
        uintptr_t FoundFunction = 0;
        for (int32_t Index = 0; Index < 0x200; ++Index)
        {
            const uintptr_t Function = VTableEntry(VTable, Index);
            if (!Function)
            {
                break;
            }
            const uintptr_t End = Finder::FunctionEnd(Function);
            if (End <= Function || End - Function >= MaxSize || Function == FoundFunction)
            {
                continue;
            }
            const bool bReferencesAll = std::all_of(Offsets.begin(), Offsets.end(), [&](ptrdiff_t Offset) { return ContainsDisp32(Function, End, static_cast<int32_t>(Offset)); });
            if (bReferencesAll && (!bNeedsCall || FindRawCall(Function, End)))
            {
                if (FoundFunction)
                {
                    return -1;
                }
                Found = Index;
                FoundFunction = Function;
            }
        }
        return Found;
    }
}

uint8_t* Finder::AbilityEndCleanupCall(ptrdiff_t AbilityGrantedWeaponItemOffset)
{
    const uintptr_t EndAbility = Finder::FunctionStart(OnlyRef(L"%s EndAbility"));
    const uintptr_t EndAbilityEnd = EndAbility ? Finder::FunctionEnd(EndAbility) : 0;
    uintptr_t Found = 0;
    int32_t Count = 0;
    for (const auto& [Call, Target] : EndAbilityEnd ? Finder::DirectCalls(EndAbility, EndAbilityEnd) : decltype(Finder::DirectCalls(0, 0)){})
    {
        const uintptr_t TargetEnd = Finder::FunctionStart(Target) == Target ? Finder::FunctionEnd(Target) : 0;
        if (TargetEnd > Target && TargetEnd - Target < 0x400 && ContainsDisp32(Target, TargetEnd, static_cast<int32_t>(AbilityGrantedWeaponItemOffset)))
        {
            Found = Call;
            ++Count;
        }
    }

    if (Count != 1)
    {
        Log::Error(L"Finder: UFortGameplayAbility::EndAbility's ability weapon cleanup call not found ({} candidates)", Count);
        return nullptr;
    }
    Log::Info(L"Finder: UFortGameplayAbility::EndAbility's ability weapon cleanup call is at 0x{:X}", Found - Finder::ImageBase());
    return reinterpret_cast<uint8_t*>(Found);
}

bool Finder::ResolveConditionalFoundationQuotaLive(ptrdiff_t QuotaOffset, FConditionalFoundationQuotaSites& OutSites)
{
    const uintptr_t Tick = reinterpret_cast<uintptr_t>(Natives::AFortWorldManager_ServerTickLoadingOrCreatingWorld);
    const uintptr_t TickEnd = Tick ? Finder::FunctionEnd(Tick) : 0;
    std::vector<uintptr_t> Checked;
    uintptr_t Quota = 0;
    int32_t Count = 0;
    for (const auto& [Call, Target] : TickEnd && QuotaOffset > 0 ? Finder::DirectCalls(Tick, TickEnd) : decltype(Finder::DirectCalls(0, 0)){})
    {
        if (std::find(Checked.begin(), Checked.end(), Target) != Checked.end() || Finder::FunctionStart(Target) != Target)
        {
            continue;
        }
        Checked.push_back(Target);
        if (ContainsDisp32(Target, Target + 0x60, static_cast<int32_t>(QuotaOffset + 8)))
        {
            Quota = Target;
            ++Count;
        }
    }

    const uintptr_t QuotaEnd = Count == 1 ? Finder::FunctionEnd(Quota) : 0;
    uintptr_t ContextCall = 0;
    uintptr_t SelectCall = 0;
    std::vector<uintptr_t> FrameLeaCalls;
    for (const auto& [Call, Target] : QuotaEnd ? Finder::DirectCalls(Quota, QuotaEnd) : decltype(Finder::DirectCalls(0, 0)){})
    {
        if (Byte(Target) == 0x33 && Byte(Target + 1) == 0xC0 && Byte(Target + 2) == 0xC3)
        {
            ContextCall = ContextCall ? UINTPTR_MAX : Call;
        }
        else if (Byte(Target) == 0x32 && Byte(Target + 1) == 0xC0 && Byte(Target + 2) == 0xC3)
        {
            SelectCall = SelectCall ? UINTPTR_MAX : Call;
        }
        else if (ContextCall && !SelectCall && Byte(Call - 4) == 0x48 && Byte(Call - 3) == 0x8D && Byte(Call - 2) == 0x4D)
        {
            FrameLeaCalls.push_back(Call);
        }
    }

    int32_t RequestSize = 0;
    uintptr_t RequestSizeSite = 0;
    for (uintptr_t Site = FrameLeaCalls.size() >= 2 && SelectCall != UINTPTR_MAX ? FrameLeaCalls[1] : 0; Site && Site + 7 <= SelectCall && !RequestSize; ++Site)
    {
        if ((Byte(Site) == 0x48 || Byte(Site) == 0x4C) && Byte(Site + 1) == 0x69 && (Byte(Site + 2) & 0xC0) == 0xC0)
        {
            RequestSize = *reinterpret_cast<const int32_t*>(Site + 3);
            RequestSizeSite = Site;
        }
    }

    // The selection takes the address of a struct whose request array follows an 8-byte field: the array element's
    // "add rcx, [rsp+X]" after the imul and the selection's "lea rdx, [rsp+Y]" give the array at +(X - Y).
    ptrdiff_t RequestsOffset = -1;
    for (uintptr_t Site = RequestSizeSite; Site && Site + 5 <= RequestSizeSite + 0x10 && RequestsOffset < 0; ++Site)
    {
        if (Byte(Site) == 0x48 && Byte(Site + 1) == 0x03 && Byte(Site + 2) == 0x4C && Byte(Site + 3) == 0x24)
        {
            RequestsOffset = Byte(Site + 4);
        }
    }
    ptrdiff_t RequestsArgument = -1;
    for (uintptr_t Site = SelectCall && SelectCall != UINTPTR_MAX ? SelectCall - 5 : 0; Site && Site > SelectCall - 0x90 && RequestsArgument < 0; --Site)
    {
        if (Byte(Site) == 0x48 && Byte(Site + 1) == 0x8D && Byte(Site + 2) == 0x54 && Byte(Site + 3) == 0x24)
        {
            RequestsArgument = Byte(Site + 4);
        }
    }
    const ptrdiff_t RequestsArrayOffset = RequestsOffset >= 0 && RequestsArgument >= 0 ? RequestsOffset - RequestsArgument : -1;

    const ptrdiff_t ClassesOffset = FrameLeaCalls.size() >= 2 ? static_cast<ptrdiff_t>(static_cast<int8_t>(Byte(FrameLeaCalls[1] - 1))) - static_cast<int8_t>(Byte(FrameLeaCalls[0] - 1)) : -1;
    if (Count != 1 || !ContextCall || ContextCall == UINTPTR_MAX || !SelectCall || SelectCall == UINTPTR_MAX || ClassesOffset <= 0 || RequestSize <= ClassesOffset || RequestsArrayOffset < 0)
    {
        Log::Error(L"Finder: AFortWorldManager conditional foundation quota calls not found ({} candidates)", Count);
        return false;
    }

    uintptr_t QuotaCall = 0;
    for (const auto& [Call, Target] : Finder::DirectCalls(Tick, TickEnd))
    {
        if (Target == Quota)
        {
            QuotaCall = QuotaCall ? UINTPTR_MAX : Call;
        }
    }

    ptrdiff_t TierIndexOffset = -1;
    for (uintptr_t Site = Quota; Site + 7 <= Quota + 0x40 && TierIndexOffset < 0; ++Site)
    {
        if ((Byte(Site) & 0xF8) == 0x48 && Byte(Site + 1) == 0x63 && (Byte(Site + 2) & 0xC0) == 0x80)
        {
            TierIndexOffset = *reinterpret_cast<const int32_t*>(Site + 3);
        }
    }

    OutSites.Quota = Quota;
    OutSites.QuotaCall = QuotaCall != UINTPTR_MAX ? reinterpret_cast<uint8_t*>(QuotaCall) : nullptr;
    OutSites.TierIndexOffset = TierIndexOffset;
    OutSites.ContextCall = reinterpret_cast<uint8_t*>(ContextCall);
    OutSites.SelectCall = reinterpret_cast<uint8_t*>(SelectCall);
    OutSites.RequestSize = RequestSize;
    OutSites.RequestClassesOffset = ClassesOffset;
    OutSites.RequestsArrayOffset = RequestsArrayOffset;
    Log::Info(L"Finder: AFortWorldManager conditional foundation quota at 0x{:X} (context call 0x{:X}, selection call 0x{:X}, request 0x{:X} bytes, classes +0x{:X}, requests +0x{:X}, tick call 0x{:X}, tier index +0x{:X})", Quota - Finder::ImageBase(), ContextCall - Finder::ImageBase(), SelectCall - Finder::ImageBase(), RequestSize, ClassesOffset, RequestsArrayOffset, OutSites.QuotaCall ? QuotaCall - Finder::ImageBase() : 0, TierIndexOffset);
    return true;
}

bool Finder::ResolvePlayerSpawnQueriesLive(FPlayerSpawnQuerySites& OutSites)
{
    const uintptr_t ChoosePlayerStart = FunctionOf(L"ChoosePlayerStart: Could not find spot for Player! Spawning at engine PlayerStart!");
    const uintptr_t ChoosePlayerStartEnd = ChoosePlayerStart ? Finder::FunctionEnd(ChoosePlayerStart) : 0;
    std::unordered_map<uintptr_t, std::vector<uintptr_t>> CandidateCalls;
    for (const auto& [Call, Target] : ChoosePlayerStartEnd ? Finder::DirectCalls(ChoosePlayerStart, ChoosePlayerStartEnd) : std::vector<std::pair<uintptr_t, uintptr_t>>())
    {
        if (Finder::FunctionStart(Target) == Target && FindBackward(Call, 0x30, { 0x4C, 0x8D, 0x4C, 0x24 }))
        {
            CandidateCalls[Target].push_back(Call);
        }
    }

    uintptr_t PickSpawnActor = 0;
    for (const auto& [Target, Calls] : CandidateCalls)
    {
        if (Calls.size() == 4)
        {
            PickSpawnActor = PickSpawnActor ? UINTPTR_MAX : Target;
        }
    }

    std::vector<uintptr_t> PickCalls = PickSpawnActor && PickSpawnActor != UINTPTR_MAX ? CandidateCalls[PickSpawnActor] : std::vector<uintptr_t>();
    std::sort(PickCalls.begin(), PickCalls.end());
    int32_t FriendlyPawnsIndex = -1;
    int32_t FriendlyPawnsCalls = 0;
    const std::initializer_list<int32_t> VirtualCall = { 0xFF, 0x90, -1, -1, -1, -1 };
    for (uintptr_t Site = PickCalls.size() == 4 ? FindInstruction(ChoosePlayerStart, PickCalls[0], VirtualCall) : 0; Site; Site = FindInstruction(Site + 1, PickCalls[0], VirtualCall))
    {
        if (FindBackward(Site, 0x30, { 0x4C, 0x8D, 0x44, 0x24 }))
        {
            FriendlyPawnsIndex = VirtualSlot(Site);
            ++FriendlyPawnsCalls;
        }
    }

    if (PickCalls.size() != 4 || FriendlyPawnsCalls != 1 || FriendlyPawnsIndex < 0)
    {
        Log::Error(L"Finder: AFortGameModeZone::ChoosePlayerStart spawn queries not found ({} spawn actor calls, {} friendly pawn calls)", PickCalls.size(), FriendlyPawnsCalls);
        return false;
    }

    OutSites.PickSpawnActor = PickSpawnActor;
    OutSites.FriendlyCalls[0] = reinterpret_cast<uint8_t*>(PickCalls[0]);
    OutSites.FriendlyCalls[1] = reinterpret_cast<uint8_t*>(PickCalls[1]);
    OutSites.InitialCall = reinterpret_cast<uint8_t*>(PickCalls[2]);
    OutSites.BackupCall = reinterpret_cast<uint8_t*>(PickCalls[3]);
    OutSites.FriendlyPawnsIndex = FriendlyPawnsIndex;
    Log::Info(L"Finder: AFortGameModeZone::ChoosePlayerStart spawn queries (spawn actor 0x{:X}, calls 0x{:X} 0x{:X} 0x{:X} 0x{:X}, friendly pawns slot 0x{:X})", PickSpawnActor - Finder::ImageBase(), PickCalls[0] - Finder::ImageBase(), PickCalls[1] - Finder::ImageBase(), PickCalls[2] - Finder::ImageBase(), PickCalls[3] - Finder::ImageBase(), FriendlyPawnsIndex);
    return true;
}

bool Finder::ResolveAbilityWeaponLive(ptrdiff_t CurrentWeaponListOffset, ptrdiff_t LastEquippedWeaponGuidOffset)
{
    if (Natives::AFortPlayerPawn_EquipLastWeaponIndex >= 0 && Natives::AFortPawn_EquipDefaultWeaponIndex >= 0)
    {
        return true;
    }

    const int32_t EquipLastIndex = UniqueSlotReferencing(CdoVTable(L"/Script/FortniteGame.FortPlayerPawn"), 0x200, { LastEquippedWeaponGuidOffset }, true);
    const int32_t EquipDefaultIndex = UniqueSlotReferencing(CdoVTable(L"/Script/FortniteGame.FortPawn"), 0x40, { CurrentWeaponListOffset, CurrentWeaponListOffset + 8 }, true);
    if (EquipLastIndex < 0 || EquipDefaultIndex < 0)
    {
        Log::Error(L"Finder: AFortPlayerPawn::EquipLastWeapon (slot {}) / AFortPawn::EquipDefaultWeapon (slot {}) not found", EquipLastIndex, EquipDefaultIndex);
        return false;
    }

    Natives::AFortPlayerPawn_EquipLastWeaponIndex = EquipLastIndex;
    Natives::AFortPawn_EquipDefaultWeaponIndex = EquipDefaultIndex;
    Log::Info(L"Finder: AFortPlayerPawn::EquipLastWeapon is slot 0x{:X}, AFortPawn::EquipDefaultWeapon is slot 0x{:X}", EquipLastIndex, EquipDefaultIndex);
    return true;
}

bool Finder::ResolveWeaponAmmoLive(ptrdiff_t AmmoCountOffset, ptrdiff_t ItemEntryGuidOffset)
{
    if (Natives::AFortWeapon_ModAmmoIndex >= 0)
    {
        return true;
    }

    const int32_t Index = UniqueSlotReferencing(CdoVTable(L"/Script/FortniteGame.FortWeapon"), 0x80, { AmmoCountOffset, ItemEntryGuidOffset }, true);
    if (Index < 0)
    {
        Log::Error(L"Finder: AFortWeapon::ModAmmo slot not found");
        return false;
    }

    Natives::AFortWeapon_ModAmmoIndex = Index;
    Log::Info(L"Finder: AFortWeapon::ModAmmo is slot 0x{:X}", Index);
    return true;
}

bool Finder::ResolveDurabilityLive(UFunction* ServerModDurability, UObject* PlayerControllerDefault, ptrdiff_t EntryDurabilityOffset, ptrdiff_t PendingDestroyDueToDurabilityOffset, uint8_t PendingDestroyDueToDurabilityMask)
{
    if (Natives::IFortInventoryOwnerInterface_ModDurabilityIndex >= 0 && Natives::UFortWorldItem_SetDurabilityIndex >= 0 && Natives::AFortWeapon_DestroyedDueToDurabilityIndex >= 0)
    {
        return true;
    }

    void** PlayerControllerVTable = PlayerControllerDefault ? *reinterpret_cast<void***>(PlayerControllerDefault) : nullptr;
    const uintptr_t ModImplementation = VTableEntry(PlayerControllerVTable, ImplementationIndex(ServerModDurability));
    const uintptr_t InterfaceAdjust = ModImplementation ? FindInstruction(ModImplementation, ModImplementation + 0x20, { 0x48, 0x81, 0xC1 }) : 0;
    int32_t ModIndex = -1;
    if (InterfaceAdjust && *reinterpret_cast<const int32_t*>(InterfaceAdjust + 3) == Natives::AFortPlayerController_InventoryOwnerInterface)
    {
        if (const uintptr_t ShortCall = FindInstruction(InterfaceAdjust, InterfaceAdjust + 0x20, { 0xFF, 0x50 }))
        {
            ModIndex = Byte(ShortCall + 2) % 8 == 0 ? Byte(ShortCall + 2) / 8 : -1;
        }
        else
        {
            ModIndex = VirtualSlot(FindVirtualCall(InterfaceAdjust, InterfaceAdjust + 0x20));
        }
    }

    UFunction* CreateTemporaryItemInstanceFunction = FindObject<UFunction>(nullptr, L"/Script/FortniteGame.FortItemDefinition:CreateTemporaryItemInstanceBP");
    const uintptr_t Exec = CreateTemporaryItemInstanceFunction ? reinterpret_cast<uintptr_t>(CreateTemporaryItemInstanceFunction->GetNativeFunc()) : 0;
    const uintptr_t ExecEnd = IsInText(Exec) ? Finder::FunctionEnd(Exec) : 0;
    uintptr_t CreateTemporaryItemInstance = 0;
    for (uintptr_t Call = ExecEnd ? FindRawCall(Exec, ExecEnd) : 0; Call; Call = FindRawCall(Call + 1, ExecEnd))
    {
        CreateTemporaryItemInstance = Finder::RelativeTarget(Call, 1, 5);
    }
    const uintptr_t CreateTemporaryItemInstanceEnd = IsInText(CreateTemporaryItemInstance) ? Finder::FunctionEnd(CreateTemporaryItemInstance) : 0;
    // CreateItem is the last virtual call: 1.7.2 also calls slot 0x53 on the level first.
    int32_t CreateItemIndex = -1;
    for (uintptr_t Call = CreateTemporaryItemInstanceEnd ? FindVirtualCall(CreateTemporaryItemInstance, CreateTemporaryItemInstanceEnd) : 0; Call; Call = FindVirtualCall(Call + 1, CreateTemporaryItemInstanceEnd))
    {
        CreateItemIndex = VirtualSlot(Call);
    }
    const uintptr_t CreateItem = VTableEntry(CdoVTable(L"/Script/FortniteGame.FortWorldItemDefinition"), CreateItemIndex);
    const uintptr_t CreateItemEnd = CreateItem ? Finder::FunctionEnd(CreateItem) : 0;
    int32_t SetDurabilityIndex = -1;
    bool bSetDurabilityAmbiguous = false;
    for (const auto& [Call, Target] : CreateItemEnd > CreateItem ? Finder::DirectCalls(CreateItem, CreateItemEnd) : std::vector<std::pair<uintptr_t, uintptr_t>>{})
    {
        const uintptr_t TargetEnd = Finder::FunctionStart(Target) == Target ? Finder::FunctionEnd(Target) : 0;
        for (uintptr_t Site = Target; TargetEnd > Target && TargetEnd - Target < 0x200 && Site + 5 <= TargetEnd; ++Site)
        {
            const uint8_t ModRm = Byte(Site + 3);
            if (Byte(Site) != 0xF3 || Byte(Site + 1) != 0x0F || Byte(Site + 2) != 0x10 || (ModRm & 0xF8) != 0x48 || ModRm == 0x4C || Byte(Site + 4) != EntryDurabilityOffset)
            {
                continue;
            }
            const int32_t Index = VirtualSlot(FindVirtualCall(Site + 5, std::min(Site + 0x18, TargetEnd)));
            if (Index >= 0)
            {
                bSetDurabilityAmbiguous |= SetDurabilityIndex >= 0 && SetDurabilityIndex != Index;
                SetDurabilityIndex = Index;
            }
        }
    }

    const uintptr_t SetDurability = VTableEntry(CdoVTable(L"/Script/FortniteGame.FortWorldItem"), SetDurabilityIndex);
    const bool bSetDurabilityStripped = SetDurability && (Byte(SetDurability) == 0x32 || Byte(SetDurability) == 0x33) && Byte(SetDurability + 1) == 0xC0 && Byte(SetDurability + 2) == 0xC3;

    void** WeaponVTable = CdoVTable(L"/Script/FortniteGame.FortWeapon");
    int32_t DestroyedIndex = -1;
    uintptr_t DestroyedFunction = 0;
    bool bDestroyedAmbiguous = false;
    for (int32_t Index = 0; Index < 0x200; ++Index)
    {
        const uintptr_t Function = VTableEntry(WeaponVTable, Index);
        if (!Function)
        {
            break;
        }
        const uintptr_t End = Function + 0x18;
        if (Function == DestroyedFunction || Byte(Function) != 0x48 || Byte(Function + 1) != 0x8B || Byte(Function + 2) != 0x01)
        {
            continue;
        }
        for (uintptr_t Site = Function + 3; Site + 7 <= Function + 0x0A; ++Site)
        {
            if ((Byte(Site) == 0x83 || Byte(Site) == 0x80) && Byte(Site + 1) == 0x89 && *reinterpret_cast<const int32_t*>(Site + 2) == PendingDestroyDueToDurabilityOffset
                && Byte(Site + 6) == PendingDestroyDueToDurabilityMask && FindVirtualCall(Site + 7, End))
            {
                bDestroyedAmbiguous |= DestroyedFunction != 0;
                DestroyedIndex = Index;
                DestroyedFunction = Function;
                break;
            }
        }
    }

    if (ModIndex < 0 || SetDurabilityIndex < 0 || bSetDurabilityAmbiguous || !bSetDurabilityStripped || DestroyedIndex < 0 || bDestroyedAmbiguous)
    {
        Log::Error(L"Finder: durability natives not found (ModDurability {}, SetDurability {} stripped {}, DestroyedDueToDurability {})", ModIndex, SetDurabilityIndex, bSetDurabilityStripped, DestroyedIndex);
        return false;
    }

    Natives::IFortInventoryOwnerInterface_ModDurabilityIndex = ModIndex;
    Natives::UFortWorldItem_SetDurabilityIndex = SetDurabilityIndex;
    Natives::AFortWeapon_DestroyedDueToDurabilityIndex = DestroyedIndex;
    Log::Info(L"Finder: IFortInventoryOwnerInterface::ModDurability slot 0x{:X}, UFortWorldItem::SetDurability slot 0x{:X}, AFortWeapon::DestroyedDueToDurability slot 0x{:X}", ModIndex, SetDurabilityIndex, DestroyedIndex);
    return true;
}

bool Finder::ResolveOutpostMcpRequestLive()
{
    if (Natives::FDelegateBase_Allocate && Natives::AFortPlayerControllerOutpost_GetFabricatorProgress)
    {
        return true;
    }

    const uintptr_t Abort = FunctionOf(L"AFortPlayerControllerOutpost::ServerAbortDisintegration");
    const uintptr_t AbortEnd = Abort ? Finder::FunctionEnd(Abort) : 0;
    uintptr_t Completed = 0;
    uintptr_t VTable = 0;
    int32_t DataLoads = 0;
    for (uintptr_t Site = Abort; Site && Site + 7 <= AbortEnd && !VTable; ++Site)
    {
        if (Byte(Site) != 0x48 || Byte(Site + 1) != 0x8D || Byte(Site + 2) != 0x05)
        {
            continue;
        }
        const uintptr_t Target = Finder::RelativeTarget(Site, 3, 7);
        if (IsInText(Target))
        {
            Completed = Completed ? Completed : Target;
        }
        else if (Completed && ++DataLoads == 2)
        {
            VTable = Target;
        }
    }

    const uintptr_t HandleStore = Abort ? FindInstruction(Abort, AbortEnd, { 0x0F, 0x11, -1, 0x10, 0xE8 }) : 0;
    const uintptr_t Copy = Abort ? FindInstruction(Abort, AbortEnd, { 0x48, 0x8D, 0x55, -1, 0xFF, 0x50, -1 }) : 0;
    uintptr_t UrlLoad = 0;
    for (uintptr_t Reference : Finder::StringRefs(L"McpDedicatedServerCommandUrl"))
    {
        UrlLoad = Reference >= Abort && Reference < AbortEnd ? Reference : UrlLoad;
    }
    uintptr_t UrlName = 0;
    for (uintptr_t Site = UrlLoad ? UrlLoad - 0x30 : 0; Site && Site < UrlLoad; ++Site)
    {
        UrlName = Byte(Site) == 0x48 && Byte(Site + 1) == 0x8D && Byte(Site + 2) == 0x4D ? Site : UrlName;
    }
    const uintptr_t Credentials = UrlLoad ? FindInstruction(UrlLoad, AbortEnd, { 0xC7, 0x45, -1, 0x01, 0x00, 0x00, 0x00 }) : 0;
    uintptr_t Context = 0;
    for (uintptr_t Site = Credentials; Site && (Site = FindInstruction(Site + 1, AbortEnd, { 0x48, 0x8D, 0x55, -1, 0x48, 0x8B, -1, 0xE8 })) != 0;)
    {
        Context = Site;
    }

    const uintptr_t CreateCopy = VTable && Copy ? VTableEntry(reinterpret_cast<void**>(VTable), Byte(Copy + 6) / 8) : 0;
    const uintptr_t AllocateCall = CreateCopy ? FindRawCall(CreateCopy, CreateCopy + 0x40) : 0;
    const uintptr_t CompletedEnd = Completed ? Finder::FunctionEnd(Completed) : 0;
    const auto CompletedCalls = CompletedEnd ? Finder::DirectCalls(Completed, CompletedEnd) : std::vector<std::pair<uintptr_t, uintptr_t>>();
    if (!Completed || !VTable || !HandleStore || !Copy || !UrlName || !Credentials || !Context || !AllocateCall || CompletedCalls.size() < 3)
    {
        Log::Error(L"Finder: outpost MCP request not decoded (completed {}, vtable {}, handle {}, copy {}, url {}, credentials {}, context {}, allocate {}, completed calls {})",
            Completed != 0, VTable != 0, HandleStore != 0, Copy != 0, UrlName != 0, Credentials != 0, Context != 0, AllocateCall != 0, CompletedCalls.size());
        return false;
    }

    const int32_t Base = static_cast<int8_t>(Byte(Context + 3));
    Natives::FBaseUrlContext_Delegate = static_cast<int8_t>(Byte(Copy + 3)) - Base;
    Natives::FBaseUrlContext_UrlConfigName = static_cast<int8_t>(Byte(UrlName + 3)) - Base;
    Natives::FBaseUrlContext_ContextCredentials = static_cast<int8_t>(Byte(Credentials + 2)) - Base;
    Natives::McpRequestDelegateInstanceVTable = reinterpret_cast<void*>(VTable);
    Natives::FDelegateBase_Allocate = reinterpret_cast<decltype(Natives::FDelegateBase_Allocate)>(Finder::RelativeTarget(AllocateCall, 1, 5));
    Natives::FDelegateHandle_GenerateNewID = reinterpret_cast<decltype(Natives::FDelegateHandle_GenerateNewID)>(Finder::RelativeTarget(HandleStore + 4, 1, 5));
    Natives::FDateTime_UtcNow = reinterpret_cast<decltype(Natives::FDateTime_UtcNow)>(CompletedCalls[CompletedCalls.size() - 2].second);
    Natives::AFortPlayerControllerOutpost_GetFabricatorProgress = reinterpret_cast<decltype(Natives::AFortPlayerControllerOutpost_GetFabricatorProgress)>(CompletedCalls.back().second);
    Log::Info(L"Finder: FBaseUrlContext delegate +0x{:X}, url config +0x{:X}, credentials +0x{:X}", Natives::FBaseUrlContext_Delegate, Natives::FBaseUrlContext_UrlConfigName, Natives::FBaseUrlContext_ContextCredentials);
    return true;
}

bool Finder::IsStubFunction(uintptr_t Function)
{
    if (!IsInText(Function))
    {
        return false;
    }
    if (reinterpret_cast<void*>(Function) == Natives::SharedEmptyFunction)
    {
        return true;
    }

    // ret / ret 0 / xor al,al / xor eax,eax / mov al,1 / xorps xmm0,xmm0, each followed by ret
    const uint8_t B0 = Byte(Function);
    const uint8_t B1 = Byte(Function + 1);
    const uint8_t B2 = Byte(Function + 2);
    return B0 == 0xC3
        || (B0 == 0xC2 && B1 == 0x00 && B2 == 0x00)
        || (B0 == 0x32 && B1 == 0xC0 && B2 == 0xC3)
        || (B0 == 0x33 && B1 == 0xC0 && B2 == 0xC3)
        || (B0 == 0xB0 && B1 == 0x01 && B2 == 0xC3)
        || (B0 == 0x0F && B1 == 0x57 && B2 == 0xC0 && Byte(Function + 3) == 0xC3);
}

bool Finder::ResolveLootLive()
{
    const uintptr_t GetItemLevel = FunctionOf(L"UFortLootLevel::GetItemLevel");

    // The interface exec ends in `call [rax+disp8]` on the interface vtable.
    UFunction* GetOwnedGameplayTagsFunction = FindObject<UFunction>(nullptr, L"/Script/GameplayTags.GameplayTagAssetInterface:GetOwnedGameplayTags");
    const uintptr_t Exec = GetOwnedGameplayTagsFunction ? reinterpret_cast<uintptr_t>(GetOwnedGameplayTagsFunction->GetNativeFunc()) : 0;
    const uintptr_t ExecEnd = IsInText(Exec) ? Finder::FunctionEnd(Exec) : 0;
    int32_t GetOwnedGameplayTagsIndex = -1;
    for (uintptr_t Site = ExecEnd ? FindInstruction(Exec, ExecEnd, { 0xFF, 0x50, -1 }) : 0; Site; Site = FindInstruction(Site + 1, ExecEnd, { 0xFF, 0x50, -1 }))
    {
        GetOwnedGameplayTagsIndex = Byte(Site + 2) % 8 == 0 ? Byte(Site + 2) / 8 : -1;
    }

    // Each is kept on its own, so the item levels don't depend on the tag slot.
    if (GetItemLevel)
    {
        Natives::UFortLootLevel_GetItemLevel = reinterpret_cast<decltype(Natives::UFortLootLevel_GetItemLevel)>(GetItemLevel);
    }
    if (GetOwnedGameplayTagsIndex >= 0)
    {
        Natives::IGameplayTagAssetInterface_GetOwnedGameplayTagsIndex = GetOwnedGameplayTagsIndex;
    }
    if (!GetItemLevel || GetOwnedGameplayTagsIndex < 0)
    {
        Log::Error(L"Finder: loot natives not found (UFortLootLevel::GetItemLevel {}, IGameplayTagAssetInterface::GetOwnedGameplayTags {})", GetItemLevel != 0, GetOwnedGameplayTagsIndex);
        return false;
    }
    Log::Info(L"Finder: UFortLootLevel::GetItemLevel +0x{:X}, IGameplayTagAssetInterface::GetOwnedGameplayTags slot 0x{:X}", GetItemLevel - Finder::ImageBase(), GetOwnedGameplayTagsIndex);
    return true;
}

bool Finder::ResolveBuildingLootLive()
{
    if (Natives::ABuildingContainer_SpawnLootIndex >= 0)
    {
        return true;
    }

    const auto LastByteSlot = [](UFunction* Function)
    {
        const uintptr_t Exec = Function ? reinterpret_cast<uintptr_t>(Function->GetNativeFunc()) : 0;
        const uintptr_t ExecEnd = IsInText(Exec) ? Finder::FunctionEnd(Exec) : 0;
        int32_t Slot = -1;
        for (uintptr_t Site = ExecEnd ? FindInstruction(Exec, ExecEnd, { 0xFF, 0x50, -1 }) : 0; Site; Site = FindInstruction(Site + 1, ExecEnd, { 0xFF, 0x50, -1 }))
        {
            Slot = Byte(Site + 2) % 8 == 0 ? Byte(Site + 2) / 8 : -1;
        }
        return Slot;
    };
    const auto Calls = [](uintptr_t Function)
    {
        return Function ? Finder::DirectCalls(Function, Finder::FunctionEnd(Function)) : std::vector<std::pair<uintptr_t, uintptr_t>>();
    };

    // ABuildingSMActor::SelectMeshSet(int32 LootTier) survives in 1.2 and ends in the SetMeshSet virtual.
    const uintptr_t SelectMeshSet = FunctionOf(L"ABuildingSMActor::SelectMeshSet(int32 LootTier) just set AltMeshIdx Building: %s, AltMeshIdx: %d");
    const uintptr_t SelectMeshSetEnd = SelectMeshSet ? Finder::FunctionEnd(SelectMeshSet) : 0;
    int32_t SetMeshSetIndex = -1;
    for (uintptr_t Site = SelectMeshSetEnd ? FindVirtualCall(SelectMeshSet, SelectMeshSetEnd) : 0; Site; Site = FindVirtualCall(Site + 1, SelectMeshSetEnd))
    {
        SetMeshSetIndex = VirtualSlot(Site);
    }

    // ABuildingSMActor::PostUpdate: Cert selects the alternate mesh set in it, 1.2 only logs.
    const uintptr_t PostUpdate = FunctionOf(L"ABuildingSMActor::PostUpdate() Building: %s, AltMeshIdx: %d");
    void** SMActorVTable = CdoVTable(L"/Script/FortniteGame.BuildingSMActor");
    const int32_t PostUpdateIndex = FirstVTableIndex(SMActorVTable, PostUpdate);
    bool bPostUpdateSelectsMeshSet = false;
    for (const auto& [Site, Target] : Calls(PostUpdate))
    {
        bPostUpdateSelectsMeshSet |= Target == SelectMeshSet;
    }

    // SpawnLoot: the first virtual call in the container's IFortInteractInterface::ServerOnAttemptInteract.
    if (!Natives::UObject_GetInterfaceAddress)
    {
        ResolveSpottingLive(FindObject<UClass>(nullptr, L"/Script/FortniteGame.FortSpottableActorInterface"));
    }
    UClass* InteractInterface = FindObject<UClass>(nullptr, L"/Script/FortniteGame.FortInteractInterface");
    const int32_t InteractSlot = LastByteSlot(FindObject<UFunction>(nullptr, L"/Script/FortniteGame.FortInteractInterface:ServerOnAttemptInteract"));
    UClass* ContainerClass = FindObject<UClass>(nullptr, L"/Script/FortniteGame.BuildingContainer");
    UObject* ContainerDefault = ContainerClass ? ContainerClass->GetDefaultObject() : nullptr;
    void* InteractAddress = ContainerDefault && InteractInterface && Natives::UObject_GetInterfaceAddress ? Natives::UObject_GetInterfaceAddress(ContainerDefault, InteractInterface) : nullptr;
    const uintptr_t AttemptInteract = InteractAddress && InteractSlot >= 0 ? VTableEntry(*reinterpret_cast<void***>(InteractAddress), InteractSlot) : 0;
    const uintptr_t AttemptInteractEnd = AttemptInteract ? Finder::FunctionEnd(AttemptInteract) : 0;
    const int32_t SpawnLootIndex = AttemptInteractEnd ? VirtualSlot(FindVirtualCall(AttemptInteract, AttemptInteractEnd)) : -1;

    // Three slots after SpawnLoot the container hides itself and records it as searched: lea r8d, [r9+4], then a tail jmp
    // (or call) to AFortWorldManager::UpdateActorRecord.
    void** ContainerVTable = ContainerDefault ? *reinterpret_cast<void***>(ContainerDefault) : nullptr;
    const uintptr_t HideContainer = SpawnLootIndex >= 0 ? VTableEntry(ContainerVTable, SpawnLootIndex + 3) : 0;
    const uintptr_t HideContainerEnd = HideContainer ? Finder::FunctionEnd(HideContainer) : 0;
    const uintptr_t SearchedState = HideContainerEnd ? FindInstruction(HideContainer, HideContainerEnd, { 0x45, 0x8D, 0x41, 0x04 }) : 0;
    uintptr_t UpdateActorRecord = 0;
    for (uintptr_t Site = SearchedState ? SearchedState + 4 : 0; Site && !UpdateActorRecord && Site < SearchedState + 0x10 && Site + 5 <= HideContainerEnd; ++Site)
    {
        if (Byte(Site) == 0xE8 || Byte(Site) == 0xE9)
        {
            const uintptr_t Target = Finder::RelativeTarget(Site, 1, 5);
            UpdateActorRecord = IsInText(Target) && Finder::FunctionStart(Target) == Target ? Target : 0;
        }
    }

    // ABuildingContainer::PostLoad: Cert selects the mesh set of the saved loot tier key in it, 1.2 does not.
    uintptr_t PostLoad = FunctionOf(L"ABuildingContainer::PostLoad() Container: %s, ContainerLootTierKey: %s, AltMeshIdx: %d");
    if (!PostLoad)
    {
        PostLoad = FunctionOf(L"ABuildingContainer::PostLoad() Container: %s, ContainerLootTierKey: %s, ReplicatedLootTier: %d, AltMeshIdx: %d");
    }
    const int32_t PostLoadIndex = FirstVTableIndex(ContainerVTable, PostLoad);
    std::vector<uintptr_t> MeshSetSelectors;
    for (const uintptr_t Ref : Finder::StringRefs(L"No alternative meshes on %s for tier %d"))
    {
        MeshSetSelectors.push_back(Finder::FunctionStart(Ref));
    }
    bool bPostLoadSelectsMeshSet = false;
    for (const auto& [Site, Target] : Calls(PostLoad))
    {
        bPostLoadSelectsMeshSet |= std::find(MeshSetSelectors.begin(), MeshSetSelectors.end(), Target) != MeshSetSelectors.end();
    }

    // AFortPlayerStateZone::ModifyStat: of the two "INVALID STAT TYPE! %d" functions, the one FortPlayerStateZone overrides.
    // The other is AFortPlayerState's own stat setter (1.2 0xCE), the same in both vtables.
    void** PlayerStateZoneVTable = CdoVTable(L"/Script/FortniteGame.FortPlayerStateZone");
    void** PlayerStateVTable = CdoVTable(L"/Script/FortniteGame.FortPlayerState");
    int32_t ModifyStatIndex = -1;
    int32_t ModifyStatCount = 0;
    for (const uintptr_t Ref : PlayerStateVTable ? Finder::StringRefs(L"INVALID STAT TYPE! %d") : std::vector<uintptr_t>{})
    {
        const uintptr_t Function = Finder::FunctionStart(Ref);
        const int32_t Index = FirstVTableIndex(PlayerStateZoneVTable, Function);
        if (Index >= 0 && VTableEntry(PlayerStateVTable, Index) != Function)
        {
            ModifyStatIndex = Index;
            ++ModifyStatCount;
        }
    }
    ModifyStatIndex = ModifyStatCount == 1 ? ModifyStatIndex : -1;

    if (!SelectMeshSet || SetMeshSetIndex < 0 || PostUpdateIndex < 0 || SpawnLootIndex < 0 || !UpdateActorRecord || PostLoadIndex < 0 || ModifyStatIndex < 0)
    {
        Log::Error(L"Finder: building loot not found (SelectMeshSet {}, SetMeshSet {}, PostUpdate {}, SpawnLoot {}, UpdateActorRecord {}, PostLoad {}, ModifyStat {})",
            SelectMeshSet != 0, SetMeshSetIndex, PostUpdateIndex, SpawnLootIndex, UpdateActorRecord != 0, PostLoadIndex, ModifyStatIndex);
        return false;
    }

    Natives::ABuildingSMActor_SelectMeshSet = reinterpret_cast<decltype(Natives::ABuildingSMActor_SelectMeshSet)>(SelectMeshSet);
    Natives::ABuildingSMActor_SetMeshSetIndex = SetMeshSetIndex;
    Natives::ABuildingSMActor_PostUpdate = PostUpdate;
    Natives::ABuildingSMActor_PostUpdateIndex = PostUpdateIndex;
    Natives::ABuildingSMActor_bPostUpdateStripped = !bPostUpdateSelectsMeshSet;
    Natives::ABuildingContainer_PostLoad = PostLoad;
    Natives::ABuildingContainer_PostLoadIndex = PostLoadIndex;
    Natives::ABuildingContainer_bPostLoadStripped = !bPostLoadSelectsMeshSet;
    Natives::AFortWorldManager_UpdateActorRecord = reinterpret_cast<decltype(Natives::AFortWorldManager_UpdateActorRecord)>(UpdateActorRecord);
    Natives::AFortPlayerStateZone_ModifyStatIndex = ModifyStatIndex;
    Natives::ABuildingContainer_SpawnLootIndex = SpawnLootIndex;
    Log::Info(L"Finder: building loot: PostUpdate slot 0x{:X} ({}), SetMeshSet slot 0x{:X}, SpawnLoot slot 0x{:X}, PostLoad slot 0x{:X} ({}), ModifyStat slot 0x{:X}, UpdateActorRecord 0x{:X}",
        PostUpdateIndex, bPostUpdateSelectsMeshSet ? L"native" : L"stripped", SetMeshSetIndex, SpawnLootIndex, PostLoadIndex, bPostLoadSelectsMeshSet ? L"native" : L"stripped", ModifyStatIndex, UpdateActorRecord - Finder::ImageBase());
    return true;
}

bool Finder::ResolveHarvestLive(ptrdiff_t AllowResourceDropOffset, uint8_t AllowResourceDropMask)
{
    if (Natives::ABuildingSMActor_AttemptSpawnResourcesIndex >= 0)
    {
        return true;
    }

    // ABuildingSMActor::OnDamageServer ("OwnedBuildingDamaged") calls the resource spawn behind `test bAllowResourceDrop`.
    const uintptr_t OnDamageServer = Finder::FunctionStart(OnlyRef("OwnedBuildingDamaged"));
    const uintptr_t OnDamageServerEnd = OnDamageServer ? Finder::FunctionEnd(OnDamageServer) : 0;
    const uint32_t Offset = static_cast<uint32_t>(AllowResourceDropOffset);
    const std::initializer_list<int32_t> AllowResourceDropTest = { 0xF6, -1, static_cast<int32_t>(Offset & 0xFF), static_cast<int32_t>((Offset >> 8) & 0xFF), static_cast<int32_t>((Offset >> 16) & 0xFF), static_cast<int32_t>(Offset >> 24), AllowResourceDropMask };
    const uintptr_t Test = OnDamageServerEnd && AllowResourceDropOffset > 0 ? FindInstruction(OnDamageServer, OnDamageServerEnd, AllowResourceDropTest) : 0;

    // 1.7.2 moved the test into a virtual gate (slot 0x19F, 0x483350) that OnDamageServer calls right before the spawn.
    void** SMActorVTable = CdoVTable(L"/Script/FortniteGame.BuildingSMActor");
    uintptr_t After = Test;
    for (uintptr_t Site = !Test && OnDamageServerEnd && AllowResourceDropOffset > 0 ? FindVirtualCall(OnDamageServer, OnDamageServerEnd) : 0; Site && !After; Site = FindVirtualCall(Site + 1, OnDamageServerEnd))
    {
        const uintptr_t Gate = Byte(Site + 1) == 0x90 ? VTableEntry(SMActorVTable, VirtualSlot(Site)) : 0;
        const uintptr_t GateEnd = Gate && Finder::FunctionStart(Gate) == Gate ? Finder::FunctionEnd(Gate) : 0;
        After = GateEnd && FindInstruction(Gate, GateEnd, AllowResourceDropTest) ? Site + 6 : 0;
    }
    const int32_t AttemptSpawnResourcesIndex = After ? VirtualSlot(FindVirtualCall(After, After + 0x40)) : -1;

    // ABuildingSMActor::DetermineMaxResourcesToSpawn survives in 1.2.
    const uintptr_t DetermineMaxResourcesToSpawn = FunctionOf(L"ABuildingSMActor::DetermineMaxResourcesToSpawn");
    const int32_t DetermineMaxResourcesToSpawnIndex = FirstVTableIndex(SMActorVTable, DetermineMaxResourcesToSpawn);

    if (AttemptSpawnResourcesIndex < 0 || DetermineMaxResourcesToSpawnIndex < 0)
    {
        Log::Error(L"Finder: harvesting not found (AttemptSpawnResources {}, DetermineMaxResourcesToSpawn {})", AttemptSpawnResourcesIndex, DetermineMaxResourcesToSpawnIndex);
        return false;
    }
    Natives::ABuildingSMActor_DetermineMaxResourcesToSpawnIndex = DetermineMaxResourcesToSpawnIndex;
    Natives::ABuildingSMActor_AttemptSpawnResourcesIndex = AttemptSpawnResourcesIndex;
    Log::Info(L"Finder: ABuildingSMActor::AttemptSpawnResources is slot 0x{:X}, DetermineMaxResourcesToSpawn slot 0x{:X}", AttemptSpawnResourcesIndex, DetermineMaxResourcesToSpawnIndex);
    return true;
}

bool Finder::ResolveLootQuotaLive(ptrdiff_t MaximumQuotaLevelBoundOffset)
{
    if (Natives::ABuildingSMActor_GetMaximumQuotaLevelBoundIndex >= 0)
    {
        return true;
    }

    // ABuildingSMActor::GetMaximumQuotaLevelBound returns the byte through the hidden return pointer: movzx eax, byte ptr [rcx+X]; mov [rdx], al.
    void** SMActorVTable = CdoVTable(L"/Script/FortniteGame.BuildingSMActor");
    const int32_t BoundIndex = MaximumQuotaLevelBoundOffset > 0 ? FirstVTableIndexWhere(SMActorVTable, [MaximumQuotaLevelBoundOffset](uintptr_t Function)
    {
        return Byte(Function) == 0x0F && Byte(Function + 1) == 0xB6 && Byte(Function + 2) == 0x81 && *reinterpret_cast<const int32_t*>(Function + 3) == static_cast<int32_t>(MaximumQuotaLevelBoundOffset) && Byte(Function + 7) == 0x88 && Byte(Function + 8) == 0x02;
    }) : -1;

    if (BoundIndex < 2)
    {
        Log::Error(L"Finder: ABuildingSMActor loot quota slots not found (GetMaximumQuotaLevelBound {})", BoundIndex);
        return false;
    }
    Natives::ABuildingSMActor_GetMaximumQuotaLevelBoundIndex = BoundIndex;
    Log::Info(L"Finder: ABuildingSMActor loot quota slots 0x{:X}-0x{:X} (GetMaximumQuotaLevelBound 0x{:X})", BoundIndex - 2, BoundIndex + 3, BoundIndex);
    return true;
}

bool Finder::ResolveWorldLootLive(ptrdiff_t DefaultLootQuotaCategoryOffset, ptrdiff_t LootDataOffset, ptrdiff_t ConstructTileWorldOffset, uint8_t ConstructTileWorldMask, FWorldLootSites& OutSites)
{
    using FCalls = std::vector<std::pair<uintptr_t, uintptr_t>>;

    // HandleWorldQueryComplete fills the zone's quotas: lea rcx, [reg+DefaultLootQuotaCategory] right before the call.
    const uintptr_t HandleWorldQuery = FunctionOf(L"HandleWorldQueryComplete: Creating brand new zone data for %s. Current Zones = %d");
    const uintptr_t HandleWorldQueryEnd = HandleWorldQuery ? Finder::FunctionEnd(HandleWorldQuery) : 0;
    uintptr_t ZoneQuotaCall = 0;
    for (const auto& [Call, Target] : HandleWorldQueryEnd && DefaultLootQuotaCategoryOffset > 0 ? Finder::DirectCalls(HandleWorldQuery, HandleWorldQueryEnd) : FCalls{})
    {
        if ((Byte(Call - 7) & 0xFE) == 0x48 && Byte(Call - 6) == 0x8D && (Byte(Call - 5) & 0xF8) == 0x88 && (Byte(Call - 5) & 0x07) != 0x04 && *reinterpret_cast<const int32_t*>(Call - 4) == static_cast<int32_t>(DefaultLootQuotaCategoryOffset))
        {
            ZoneQuotaCall = ZoneQuotaCall ? UINTPTR_MAX : Call;
        }
    }
    const uintptr_t Quota = ZoneQuotaCall && ZoneQuotaCall != UINTPTR_MAX ? Finder::RelativeTarget(ZoneQuotaCall, 1, 5) : 0;

    // UWorldTileManager's map pick fills each tile's quotas through the same function.
    const uintptr_t TileMapPick = FunctionOf(L"Not all tiles have loot quota categories and the default category hasn't been set.");
    const uintptr_t TileMapPickEnd = TileMapPick ? Finder::FunctionEnd(TileMapPick) : 0;
    uintptr_t TileQuotaCall = 0;
    for (const auto& [Call, Target] : TileMapPickEnd && Quota ? Finder::DirectCalls(TileMapPick, TileMapPickEnd) : FCalls{})
    {
        if (Target == Quota)
        {
            TileQuotaCall = TileQuotaCall ? UINTPTR_MAX : Call;
        }
    }

    // The tick asks for each new tile's loot record (tests bConstructTileWorld, falls back to LootData). Cert then hands the
    // buildings their quota tiers from a function that checks -SpawnAllLoot; 1.2 still asks for the record but drops the rest.
    std::vector<uintptr_t> SpawnAllLootFunctions;
    for (const uintptr_t Ref : Finder::StringRefs(L"SpawnAllLoot"))
    {
        SpawnAllLootFunctions.push_back(Finder::FunctionStart(Ref));
    }
    const uint32_t Offset = static_cast<uint32_t>(ConstructTileWorldOffset);
    const std::initializer_list<int32_t> ConstructTileWorldTest = { 0xF6, -1, static_cast<int32_t>(Offset & 0xFF), static_cast<int32_t>((Offset >> 8) & 0xFF), static_cast<int32_t>((Offset >> 16) & 0xFF), static_cast<int32_t>(Offset >> 24), ConstructTileWorldMask };
    const uintptr_t Tick = reinterpret_cast<uintptr_t>(Natives::AFortWorldManager_ServerTickLoadingOrCreatingWorld);
    const uintptr_t TickEnd = Tick ? Finder::FunctionEnd(Tick) : 0;
    const FCalls TickCalls = TickEnd ? Finder::DirectCalls(Tick, TickEnd) : FCalls{};
    std::vector<uintptr_t> Checked;
    std::vector<uintptr_t> TileRecords;
    bool bSpawnsTileLoot = false;
    for (const auto& [Call, Target] : ConstructTileWorldOffset > 0 && LootDataOffset > 0 ? TickCalls : FCalls{})
    {
        if (std::find(SpawnAllLootFunctions.begin(), SpawnAllLootFunctions.end(), Target) != SpawnAllLootFunctions.end())
        {
            bSpawnsTileLoot = true;
        }
        if (std::find(Checked.begin(), Checked.end(), Target) != Checked.end() || Finder::FunctionStart(Target) != Target)
        {
            continue;
        }
        Checked.push_back(Target);
        const uintptr_t TargetEnd = Finder::FunctionEnd(Target);
        if (TargetEnd > Target && TargetEnd - Target < 0x400 && FindInstruction(Target, TargetEnd, ConstructTileWorldTest) && ContainsDisp32(Target, TargetEnd, static_cast<int32_t>(LootDataOffset)))
        {
            TileRecords.push_back(Target);
        }
    }
    uintptr_t TileRecordCall = 0;
    for (const auto& [Call, Target] : TileRecords.size() == 1 ? TickCalls : FCalls{})
    {
        if (Target == TileRecords.front())
        {
            TileRecordCall = TileRecordCall ? UINTPTR_MAX : Call;
        }
    }

    // The world manager fills its random lists from the world generation stream; the 2000-entry one is the loot list.
    const uintptr_t InitRandomLists = FunctionOf(L"World Generation random stream initialized using seed %d");
    const uintptr_t LootListSize = InitRandomLists ? FindInstruction(InitRandomLists, InitRandomLists + 0x200, { 0xC7, 0x41, 0x14, 0xD0, 0x07, 0x00, 0x00 }) : 0;
    ptrdiff_t LootRandomList = -1;
    for (uintptr_t Site = LootListSize ? LootListSize - 0x20 : 0; Site && Site + 7 <= LootListSize; ++Site)
    {
        if (Byte(Site) == 0x48 && Byte(Site + 1) == 0x8D && (Byte(Site + 2) & 0xF8) == 0x88 && (Byte(Site + 2) & 0x07) != 0x04)
        {
            LootRandomList = *reinterpret_cast<const int32_t*>(Site + 3);
        }
    }

    if (!Quota || (!bSpawnsTileLoot && (!TileRecordCall || TileRecordCall == UINTPTR_MAX || LootRandomList <= 0)))
    {
        Log::Error(L"Finder: world loot not found (quota fill 0x{:X}, {} tile record candidates, loot random list +0x{:X})", Quota ? Quota - Finder::ImageBase() : 0, TileRecords.size(), LootRandomList);
        return false;
    }

    OutSites.Quota = Quota;
    OutSites.ZoneQuotaCall = reinterpret_cast<uint8_t*>(ZoneQuotaCall);
    OutSites.TileQuotaCall = TileQuotaCall && TileQuotaCall != UINTPTR_MAX ? reinterpret_cast<uint8_t*>(TileQuotaCall) : nullptr;
    OutSites.TileRecordCall = TileRecordCall && TileRecordCall != UINTPTR_MAX ? reinterpret_cast<uint8_t*>(TileRecordCall) : nullptr;
    OutSites.bSpawnsTileLoot = bSpawnsTileLoot;
    if (OutSites.TileRecordCall)
    {
        Natives::AFortWorldManager_GetTileRecord = reinterpret_cast<decltype(Natives::AFortWorldManager_GetTileRecord)>(Finder::RelativeTarget(TileRecordCall, 1, 5));
    }
    Natives::AFortWorldManager_LootRandomList = LootRandomList;
    Log::Info(L"Finder: world loot: quota fill 0x{:X} ({}), zone call 0x{:X}, tile call 0x{:X}, tile record call 0x{:X}, tile loot {}, loot random list +0x{:X}", Quota - Finder::ImageBase(), Finder::IsStubFunction(Quota) ? L"stripped" : L"native", ZoneQuotaCall - Finder::ImageBase(), OutSites.TileQuotaCall ? TileQuotaCall - Finder::ImageBase() : 0, OutSites.TileRecordCall ? TileRecordCall - Finder::ImageBase() : 0, bSpawnsTileLoot ? L"native" : L"stripped", LootRandomList);
    return true;
}

bool Finder::ResolveSpawnGroupUpgradeLive(ptrdiff_t AvailableUpgradesOffset, ptrdiff_t ActiveSpawnGroupsOffset, ptrdiff_t TotalGroupCostOffset, ptrdiff_t PawnUpgradesOffset, ptrdiff_t ModifiersForAllPawnsOffset, FSpawnGroupUpgradeSites& OutSites)
{
    using FCalls = std::vector<std::pair<uintptr_t, uintptr_t>>;

    // The group's cost is stored right after its call: mov [reg+TotalGroupCost], eax.
    const auto FindGroupCostCall = [TotalGroupCostOffset](uintptr_t Function, uintptr_t End)
    {
        uintptr_t Found = 0;
        for (const auto& [Call, Target] : Finder::DirectCalls(Function, End))
        {
            for (uintptr_t Site = Call + 5; Site < Call + 0x10 && Site + 3 <= End; ++Site)
            {
                if (Byte(Site) == 0x89 && (Byte(Site + 1) & 0xF8) == 0x40 && (Byte(Site + 1) & 0x07) != 0x04 && Byte(Site + 2) == static_cast<uint8_t>(TotalGroupCostOffset))
                {
                    Found = Found ? UINTPTR_MAX : Call;
                    break;
                }
            }
        }
        return Found;
    };

    // Each enemy's pawn upgrade is written into the instance's PawnUpgrades: mov r9, [reg+PawnUpgrades] before the call.
    const auto FindPawnUpgradeCall = [PawnUpgradesOffset](uintptr_t Function, uintptr_t End)
    {
        uintptr_t Found = 0;
        for (const auto& [Call, Target] : Finder::DirectCalls(Function, End))
        {
            for (uintptr_t Site = Call >= Function + 0x18 ? Call - 0x18 : Function; Site + 7 <= Call; ++Site)
            {
                if (Byte(Site) == 0x4C && Byte(Site + 1) == 0x8B && (Byte(Site + 2) & 0xF8) == 0x88 && (Byte(Site + 2) & 0x07) != 0x04 && *reinterpret_cast<const int32_t*>(Site + 3) == static_cast<int32_t>(PawnUpgradesOffset))
                {
                    Found = Found ? UINTPTR_MAX : Call;
                    break;
                }
            }
        }
        return Found;
    };

    // The spawning tick creates the next group's instance, and the creation tries the upgrade: the one callee that reads
    // AvailableUpgrades and ActiveSpawnGroups and holds both call sites.
    const uintptr_t SpawningTick = FunctionOf(L"\nEncounter %s current group spawning has invalid GUID, resetting the current group spawning to select another");
    const uintptr_t SpawningTickEnd = SpawningTick ? Finder::FunctionEnd(SpawningTick) : 0;
    std::vector<uintptr_t> Checked;
    uintptr_t TryUpgrade = 0;
    uintptr_t TryUpgradeCall = 0;
    uintptr_t GroupCostCall = 0;
    uintptr_t PawnUpgradeCall = 0;
    for (const auto& [Call, Creation] : SpawningTickEnd && AvailableUpgradesOffset > 0 && ActiveSpawnGroupsOffset > 0 ? Finder::DirectCalls(SpawningTick, SpawningTickEnd) : FCalls{})
    {
        if (std::find(Checked.begin(), Checked.end(), Creation) != Checked.end() || Finder::FunctionStart(Creation) != Creation)
        {
            continue;
        }
        Checked.push_back(Creation);
        const uintptr_t CreationEnd = Finder::FunctionEnd(Creation);
        for (const auto& [CreationCall, Target] : CreationEnd > Creation ? Finder::DirectCalls(Creation, CreationEnd) : FCalls{})
        {
            const uintptr_t TargetEnd = Finder::FunctionStart(Target) == Target ? Finder::FunctionEnd(Target) : 0;
            if (TargetEnd <= Target || TargetEnd - Target > 0x1000 || !ContainsDisp32(Target, TargetEnd, static_cast<int32_t>(AvailableUpgradesOffset)) || !ContainsDisp32(Target, TargetEnd, static_cast<int32_t>(ActiveSpawnGroupsOffset)))
            {
                continue;
            }
            const uintptr_t CostCall = FindGroupCostCall(Target, TargetEnd);
            const uintptr_t UpgradeCall = FindPawnUpgradeCall(Target, TargetEnd);
            if (!CostCall || CostCall == UINTPTR_MAX || !UpgradeCall || UpgradeCall == UINTPTR_MAX)
            {
                continue;
            }
            TryUpgradeCall = TryUpgradeCall || (TryUpgrade && TryUpgrade != Target) ? UINTPTR_MAX : CreationCall;
            TryUpgrade = Target;
            GroupCostCall = CostCall;
            PawnUpgradeCall = UpgradeCall;
        }
    }

    if (!TryUpgrade || !TryUpgradeCall || TryUpgradeCall == UINTPTR_MAX)
    {
        Log::Error(L"Finder: spawn group upgrade not found (spawning tick 0x{:X}, upgrade 0x{:X}, {})", SpawningTick ? SpawningTick - Finder::ImageBase() : 0, TryUpgrade ? TryUpgrade - Finder::ImageBase() : 0, TryUpgradeCall == UINTPTR_MAX ? L"ambiguous" : L"no call");
        return false;
    }

    OutSites.TryUpgrade = TryUpgrade;
    OutSites.TryUpgradeCall = reinterpret_cast<uint8_t*>(TryUpgradeCall);
    OutSites.GroupCostCall = reinterpret_cast<uint8_t*>(GroupCostCall);
    OutSites.PawnUpgradeCall = reinterpret_cast<uint8_t*>(PawnUpgradeCall);
    OutSites.bRollsModifiers = ContainsDisp32(TryUpgrade, Finder::FunctionEnd(TryUpgrade), static_cast<int32_t>(ModifiersForAllPawnsOffset));
    Natives::UFortAIEncounterInfo_TryUpgradeSpawnGroup = reinterpret_cast<decltype(Natives::UFortAIEncounterInfo_TryUpgradeSpawnGroup)>(TryUpgrade);
    Natives::UFortAISpawnGroup_GetSpawnGroupCost = reinterpret_cast<decltype(Natives::UFortAISpawnGroup_GetSpawnGroupCost)>(Finder::RelativeTarget(GroupCostCall, 1, 5));
    Natives::FFortAISpawnGroupUpgrade_PickPawnUpgrade = reinterpret_cast<decltype(Natives::FFortAISpawnGroupUpgrade_PickPawnUpgrade)>(Finder::RelativeTarget(PawnUpgradeCall, 1, 5));
    Log::Info(L"Finder: spawn group upgrade 0x{:X} ({}), call 0x{:X}, group cost call 0x{:X}, pawn upgrade call 0x{:X}", TryUpgrade - Finder::ImageBase(), OutSites.bRollsModifiers ? L"native" : L"stripped", TryUpgradeCall - Finder::ImageBase(), GroupCostCall - Finder::ImageBase(), PawnUpgradeCall - Finder::ImageBase());
    return true;
}
