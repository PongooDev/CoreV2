#include "pch.h"
#include "Finder.h"
#include "Natives.h"
#include "Log.h"

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

    Log::Info(L"Finder: static resolution complete, {} missing", MissingCount);
    return MissingCount == 0;
}
