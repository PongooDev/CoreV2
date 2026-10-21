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

    bool IsInText(uintptr_t Address)
    {
        return Address >= Finder::TextBegin() && Address < Finder::TextEnd();
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
            if (FindInstruction(Lea + 4, Lea + 0x20, { 0x48, 0x89, 0x83, 0xE8, 0x00, 0x00, 0x00 }))
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

        ptrdiff_t BunchClose = 0;
        const uintptr_t OutBunchEnd = IsInText(OutBunch) ? Finder::FunctionEnd(OutBunch) : 0;
        const uintptr_t CloseArgument = OutBunchEnd ? FindInstruction(OutBunch, OutBunchEnd, { 0x41, 0x0F, 0xB6, -1 }) : 0;
        if (CloseArgument && (Byte(CloseArgument + 3) & 0xC7) == 0xC0)
        {
            const uint8_t Register = (Byte(CloseArgument + 3) >> 3) & 0x07;
            for (uintptr_t Store = CloseArgument + 4; Store + 6 <= OutBunchEnd && !BunchClose; ++Store)
            {
                const uint8_t ModRm = Byte(Store + 1);
                const bool bNeedsRex = Register >= 4;
                if (Byte(Store) == 0x88 && (ModRm & 0xC0) == 0x80 && ((ModRm >> 3) & 0x07) == Register && (ModRm & 0x07) != 0x04 && (!bNeedsRex || Byte(Store - 1) == 0x40))
                {
                    BunchClose = *reinterpret_cast<const int32_t*>(Store + 2);
                }
            }
        }

        ptrdiff_t BunchDormant = 0;
        ptrdiff_t BunchReliable = 0;
        const ptrdiff_t CloseBunchLocal = CloseBunch ? Byte(CloseBunch + 7) : 0;
        if (const uintptr_t Store = CloseBunch ? FindInstruction(CloseBunch + 16, CloseBunch + 0x60, { 0xC6, 0x84, 0x24, -1, -1, -1, -1, 0x01 }) : 0)
        {
            BunchReliable = *reinterpret_cast<const int32_t*>(Store + 3) - CloseBunchLocal;
        }
        if (const uintptr_t Store = CloseBunch ? FindInstruction(CloseBunch + 16, CloseBunch + 0x60, { 0x88, 0x84, 0x24 }) : 0)
        {
            BunchDormant = *reinterpret_cast<const int32_t*>(Store + 3) - CloseBunchLocal;
        }
        const bool bBunchFlags = BunchClose > 0 && BunchDormant == BunchClose + 1 && BunchReliable == BunchClose + 3;
        RequireOffset(L"FOutBunch::bClose", Natives::FOutBunch_bClose, bBunchFlags ? BunchClose : 0);
        RequireOffset(L"FOutBunch::bDormant", Natives::FOutBunch_bDormant, bBunchFlags ? BunchDormant : 0);
        RequireOffset(L"FOutBunch::bReliable", Natives::FOutBunch_bReliable, bBunchFlags ? BunchReliable : 0);

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
        Log::Info(L"Finder: FOutBunch is 0x{:X} bytes, bClose +0x{:X}, bDormant +0x{:X}, bReliable +0x{:X}, Num +0x{:X}, ArIsError +0x{:X} & 0x{:X}, UChannel::ChIndex +0x{:X}", BunchSize, BunchClose, BunchDormant, BunchReliable, WriterNum, ErrorByte, ErrorMask, ChIndex);

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
        ptrdiff_t ClientWorldPackageName = 0;
        for (uintptr_t Compare = LevelInitializedBody ? FindInstruction(LevelInitializedBody, LevelInitializedBody + 0x40, { 0x48, 0x39 }) : 0; Compare && !ClientWorldPackageName; Compare = FindInstruction(Compare + 1, LevelInitializedBody + 0x40, { 0x48, 0x39 }))
        {
            if ((Byte(Compare + 2) & 0xC0) == 0x80 && (Byte(Compare + 2) & 0x07) != 0x04)
            {
                ClientWorldPackageName = *reinterpret_cast<const int32_t*>(Compare + 3);
            }
        }
        RequireOffset(L"UNetConnection::ClientWorldPackageName", Natives::UNetConnection_ClientWorldPackageName, ClientWorldPackageName);

        const uintptr_t Describe = FunctionOf(L"[UNetConnection] RemoteAddr: %s, Name: %s, Driver: %s, IsServer: %s, PC: %s, Owner: %s");
        const uintptr_t DescribeEnd = Finder::FunctionEnd(Describe);
        void** ConnectionVTable = DescribeEnd ? CalledSlot(Describe, DescribeEnd, FunctionOf(L"UNetConnection::LowLevelGetRemoteAddress")).VTable : nullptr;

        int32_t ConnectionTick = -1;
        int32_t ConnectionTicks = 0;
        for (uintptr_t Call = TickFlushEnd ? FindInstruction(TickFlush, TickFlushEnd, { 0x48, 0x8B, 0x01, 0xFF, 0x90 }) : 0; Call; Call = FindInstruction(Call + 1, TickFlushEnd, { 0x48, 0x8B, 0x01, 0xFF, 0x90 }))
        {
            const int32_t Slot = VirtualSlot(Call + 3);
            if (Slot < 0 || (ConnectionTicks && Slot != ConnectionTick))
            {
                ConnectionTicks = 0;
                break;
            }
            ConnectionTick = Slot;
            ++ConnectionTicks;
        }
        const uintptr_t ConnectionTickBody = ConnectionTicks >= 2 ? VTableEntry(ConnectionVTable, ConnectionTick) : 0;
        RequireSlot(L"UNetConnection::Tick", Natives::UNetConnection_TickIndex, ConnectionTickBody ? ConnectionTick : -1);

        const int32_t InitRemoteConnection = Finder::VTableIndex(ConnectionVTable, reinterpret_cast<void*>(FunctionOf(L"UNetConnection::InitRemoteConnection")), 0x400);
        const int32_t IsNetReady = InitRemoteConnection - 6;
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
        RequireSlot(L"UNetConnection::ClientHasInitializedLevelFor", Natives::UNetConnection_ClientHasInitializedLevelForIndex, bVisibleLevelData && bVisibleLevelCount ? ClientHasInitializedLevelFor : -1);
        RequireOffset(L"UNetConnection::ClientVisibleLevelNames", Natives::UNetConnection_ClientVisibleLevelNames, bVisibleLevelData && bVisibleLevelCount ? VisibleLevelNames : 0);

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

        const uintptr_t WorldIsReady = UniqueFunction({ 0x83, 0x8F, 0x30, 0x05, 0x00, 0x00, 0x01 });
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
        const uintptr_t StartBranch = StartChecks == 1 ? StartCheck + 10 : 0;
        const uintptr_t StartTarget = StartBranch ? StartBranch + 11 + static_cast<int8_t>(Byte(StartBranch + 10)) : 0;
        const bool bStartCheck = StartTarget && StartBranch + 2 + static_cast<int8_t>(Byte(StartBranch + 1)) < StartTarget && StartTarget - (StartBranch + 2) <= 0x7F;
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
    const int32_t StructOffset = ObjectLayout::Current.UStructProperty_Struct;
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
    const int32_t ArrayDimOffset = ObjectLayout::Current.UProperty_ArrayDim;
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

bool Finder::ResolveInventoryOwnerLive(UFunction* ServerRemoveInventoryItem, void** PlayerControllerVTable)
{
    const uintptr_t Implementation = VTableEntry(PlayerControllerVTable, ImplementationIndex(ServerRemoveInventoryItem));
    const uintptr_t InterfaceAdjust = Implementation ? FindInstruction(Implementation, Implementation + 0x20, { 0x48, 0x81, 0xC1 }) : 0;
    const int32_t RemoveIndex = InterfaceAdjust ? VirtualSlot(FindVirtualCall(InterfaceAdjust, InterfaceAdjust + 0x20)) : -1;
    if (!InterfaceAdjust || RemoveIndex < 0)
    {
        Log::Error(L"Finder: IFortInventoryOwnerInterface::RemoveInventoryItem not found");
        return false;
    }

    Natives::AFortPlayerController_InventoryOwnerInterface = *reinterpret_cast<const int32_t*>(InterfaceAdjust + 3);
    Natives::IFortInventoryOwnerInterface_RemoveInventoryItemIndex = RemoveIndex;
    Log::Info(L"Finder: IFortInventoryOwnerInterface is AFortPlayerController +0x{:X}, RemoveInventoryItem slot 0x{:X}", Natives::AFortPlayerController_InventoryOwnerInterface, RemoveIndex);
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

    const uintptr_t CreateTemporaryItemInstance = LastCall(L"/Script/FortniteGame.FortItemDefinition:CreateTemporaryItemInstanceBP");
    const uintptr_t EntryCall = CreateTemporaryItemInstance ? FindRawCall(CreateTemporaryItemInstance, Finder::FunctionEnd(CreateTemporaryItemInstance)) : 0;
    const uintptr_t EntryConstructor = EntryCall ? Finder::RelativeTarget(EntryCall, 1, 5) : 0;

    const uintptr_t SpawnMissionItemPickup = LastCall(L"/Script/FortniteGame.FortMissionLibrary:SpawnMissionItemPickup");
    const uintptr_t SpawnMissionItemPickupEnd = SpawnMissionItemPickup ? Finder::FunctionEnd(SpawnMissionItemPickup) : 0;
    uintptr_t CreatePickup = 0;
    for (uintptr_t Call = SpawnMissionItemPickupEnd && EntryConstructor ? FindRawCall(SpawnMissionItemPickup, SpawnMissionItemPickupEnd) : 0; Call && !CreatePickup; Call = FindRawCall(Call + 1, SpawnMissionItemPickupEnd))
    {
        if (Finder::RelativeTarget(Call, 1, 5) == EntryConstructor)
        {
            const uintptr_t Next = FindRawCall(Call + 5, SpawnMissionItemPickupEnd);
            CreatePickup = Next ? Finder::RelativeTarget(Next, 1, 5) : 0;
        }
    }

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
    Log::Info(L"Finder: AFortPickup::GivePickupTo is slot 0x{:X}", GiveIndex);
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
    if (Natives::AFortMissionManager_OnZoneEndIndex >= 0 && Natives::AFortGameModeZone_OnZoneEndIndex >= 0 && Natives::AFortGameStateZone_SetGameplayStateIndex >= 0 && Natives::UFortQuestManager_SendStatEvent && Natives::UFortQuestManager_SendEndPrimaryMission && Natives::UGameInstance_GetGameSessionId && Natives::UEnum_GetEnumName)
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
    const int32_t GameStateIndex = OnRepIndex >= 0 ? FirstEntryContaining(GameStateVTable, { OnRepCall }, Contains) : -1;

    if (!SendStatEvent || !SendEndPrimaryMission || !GameSessionId || !GetEnumName || MissionManagerIndex < 0 || GameModeIndex < 0 || GameStateIndex < 0)
    {
        Log::Error(L"Finder: zone end not found (stat {}, end mission {}, session {}, enum {}, mission manager {}, game mode {}, game state {})", SendStatEvent != 0, SendEndPrimaryMission != 0, GameSessionId != 0, GetEnumName != 0, MissionManagerIndex, GameModeIndex, GameStateIndex);
        return false;
    }

    Natives::UFortQuestManager_SendStatEvent = reinterpret_cast<decltype(Natives::UFortQuestManager_SendStatEvent)>(SendStatEvent);
    Natives::UFortQuestManager_SendEndPrimaryMission = reinterpret_cast<decltype(Natives::UFortQuestManager_SendEndPrimaryMission)>(SendEndPrimaryMission);
    Natives::UGameInstance_GetGameSessionId = reinterpret_cast<decltype(Natives::UGameInstance_GetGameSessionId)>(GameSessionId);
    Natives::UEnum_GetEnumName = reinterpret_cast<decltype(Natives::UEnum_GetEnumName)>(GetEnumName);
    Natives::AFortMissionManager_OnZoneEndIndex = MissionManagerIndex;
    Natives::AFortGameModeZone_OnZoneEndIndex = GameModeIndex;
    Natives::AFortGameStateZone_SetGameplayStateIndex = GameStateIndex;
    Log::Info(L"Finder: zone end resolved: AFortMissionManager::OnZoneEnd slot 0x{:X}, AFortGameModeZone::OnZoneEnd slot 0x{:X}, AFortGameStateZone::SetGameplayState slot 0x{:X}", MissionManagerIndex, GameModeIndex, GameStateIndex);
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

        const uintptr_t CalleeEnd = Finder::FunctionEnd(Callee);
        for (uintptr_t Store = CalleeEnd ? FindInstruction(Callee, CalleeEnd, { 0x48, 0x89 }) : 0; Store; Store = FindInstruction(Store + 1, CalleeEnd, { 0x48, 0x89 }))
        {
            const uint8_t ModRm = Byte(Store + 2);
            if ((ModRm & 0xC0) == 0x80 && (ModRm & 0x07) != 0x04 && *reinterpret_cast<const int32_t*>(Store + 3) == GameStateMissionManagerOffset)
            {
                SpawnMissionManager = Callee;
                RecordConstructor = PreviousCallee;
                break;
            }
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

    void** WorldItemDefinitionVTable = CdoVTable(L"/Script/FortniteGame.FortWorldItemDefinition");
    void** GadgetVTable = CdoVTable(L"/Script/FortniteGame.FortGadgetItemDefinition");
    for (int32_t Index = 0; WorldItemDefinitionVTable && GadgetVTable && Natives::UAbilitySystemComponent_GiveAbility && Index < 0x100 && Natives::UFortWorldItemDefinition_OnItemAddedToInventoryIndex < 0; ++Index)
    {
        const uintptr_t Function = VTableEntry(GadgetVTable, Index);
        const uintptr_t FunctionEnd = Function ? Finder::FunctionEnd(Function) : 0;
        if (WorldItemDefinitionVTable[Index] == Natives::SharedEmptyFunction && FunctionEnd > Function && CallWithin(Function, FunctionEnd - Function, Natives::UAbilitySystemComponent_GiveAbility))
        {
            Natives::UFortWorldItemDefinition_OnItemAddedToInventoryIndex = Index;
        }
    }
    if (Natives::UFortWorldItemDefinition_OnItemAddedToInventoryIndex < 0)
    {
        Log::Error(L"Finder: UFortWorldItemDefinition item added slot not found");
    }
    else
    {
        Log::Info(L"Finder: UFortWorldItemDefinition item added slot 0x{:X}", Natives::UFortWorldItemDefinition_OnItemAddedToInventoryIndex);
    }
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
