#include "pch.h"
#include "AbilitySystemComponent.h"
#include "Finder.h"
#include "Natives.h"
#include "Log.h"
#include "ObjectLayout.h"
#include "Packed.h"
#include "Engine/EngineBaseTypes.h"
#include "AI/Navigation/NavigationSystem.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "Net/DataBunch.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectArray.h"
#include "UObject/UObjectGlobals.h"
#include "../../ThirdParty/MinHook/src/hde/hde64.h"

#include <algorithm>
#include <cstring>
#include <iterator>
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

    // FunctionOf, except that a reference with no function around it (a piece of the packer's moved code, which has no
    // .pdata) passes the search on to the next reference. Index is the reference used and Count how many there are.
    uintptr_t FunctionOfFirstRef(const wchar_t* Text, size_t& Index, size_t& Count)
    {
        const std::vector<uintptr_t> Refs = Finder::StringRefs(Text);
        Count = Refs.size();
        for (Index = 0; Index < Refs.size(); ++Index)
        {
            if (const uintptr_t Function = Finder::FunctionStart(Refs[Index]))
            {
                return Function;
            }
        }
        return 0;
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
        for (uintptr_t Address = Begin; Begin && Address + Length <= End; ++Address)
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

    // In a packed exe's second executable section (Finder::HasMovedCode), which holds the moved functions' pieces.
    bool IsInMovedCode(uintptr_t Address)
    {
        const auto& Sections = Finder::ExecutableSections();
        return std::any_of(Sections.begin() + 1, Sections.end(), [&](const auto& Section) { return Address >= Section.first && Address + 16 <= Section.second; });
    }

    // The shared empty function is ret 0. 1.9.1's (Finder::HasMovedCode) jumps into the second section's guard code, so
    // there it is known as the function most vtable slots hold.
    bool IsSharedEmptyBody(uintptr_t Function)
    {
        return Finder::BodyIs(Function, { 0xC2, 0x00, 0x00 }) || (Finder::HasMovedCode() && Function && Function == Finder::MostReferencedFunction());
    }

    uintptr_t UniqueFunction(std::initializer_list<int32_t> Pattern);

    // A vtable slot Core has swapped holds one of this module's functions.
    bool IsCoreFunction(uintptr_t Address)
    {
        const uintptr_t Base = reinterpret_cast<uintptr_t>(&__ImageBase);
        const IMAGE_NT_HEADERS* Headers = reinterpret_cast<const IMAGE_NT_HEADERS*>(Base + __ImageBase.e_lfanew);
        return Address >= Base && Address < Base + Headers->OptionalHeader.SizeOfImage;
    }

    uintptr_t FindRawCall(uintptr_t Begin, uintptr_t End)
    {
        for (uintptr_t Address = Begin; Begin && Address + 5 <= End; ++Address)
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
        // UE 4.21 (5.0) logs "Pre-allocating" or "Presizing" through a %s: UObjectBaseInit 0x17A69A0 on CL-4204761.
        uintptr_t UObjectBaseInit = FunctionOf(L"Presizing for max %d objects, including %i objects not considered by GC, pre-allocating %i bytes for permanent pool.");
        if (!UObjectBaseInit)
        {
            UObjectBaseInit = FunctionOf(L"%s for max %d objects, including %i objects not considered by GC, pre-allocating %i bytes for permanent pool.");
        }
        size_t AllocatePoolRef = 0;
        size_t AllocatePoolRefs = 0;
        const uintptr_t AllocatePool = FunctionOfFirstRef(L"Max UObject count is invalid. It must be a number that is greater than 0.", AllocatePoolRef, AllocatePoolRefs);
        if (AllocatePool && AllocatePoolRef > 0)
        {
            Log::Info(L"Finder: FUObjectArray::AllocateObjectPool +0x{:X} from reference {} of {} (the first is in no function)", AllocatePool - Finder::ImageBase(), AllocatePoolRef + 1, AllocatePoolRefs);
        }
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

        // AllocateObjectPool first stores MaxObjectsNotConsideredByGC, the third member: mov [rcx+8], r8d up to 1.9.1,
        // mov [rcx+34Ch], r8d on 1.10.
        if (const uintptr_t Store = AllocatePool ? FindInstruction(AllocatePool, AllocatePool + 0x20, { 0x44, 0x89, 0x81 }) : 0)
        {
            const int32_t Members = *reinterpret_cast<const int32_t*>(Store + 3) - 8;
            if (Members > 0 && Members % 4 == 0 && !FindInstruction(AllocatePool, Store, { 0x44, 0x89, 0x41 }))
            {
                ObjectLayout::Current.FUObjectArray_Members = Members;
                Log::Info(L"Object layout: FUObjectArray's members start at +0x{:X}", Members);
            }
        }
        // 4.4.0 (CL-4117433) has them 8 bytes in, close enough for a byte displacement: mov [rcx+10h], r8d (0x1579311),
        // where 4.4.2 has mov [rcx+110h], r8d. Up to 1.9.1 that store is mov [rcx+8], r8d, which leaves them at 0. 5.21
        // (CL-4288479) has them at +0x64 (mov [rcx+6Ch], r8d, 0x17DB48A), and its ObjObjects is chunked.
        if (!ObjectLayout::Current.FUObjectArray_Members)
        {
            const uintptr_t Store = AllocatePool ? FindInstruction(AllocatePool, AllocatePool + 0x20, { 0x44, 0x89, 0x41 }) : 0;
            const int32_t Members = Store ? static_cast<int8_t>(Byte(Store + 3)) - 8 : 0;
            if (Members > 0 && Members % 4 == 0 && !FindInstruction(AllocatePool, Store, { 0x44, 0x89, 0x81 }))
            {
                ObjectLayout::Current.FUObjectArray_Members = Members;
                Log::Info(L"Object layout: FUObjectArray's members start at +0x{:X}", Members);
            }
        }
        if (const int32_t Members = ObjectLayout::Current.FUObjectArray_Members)
        {
            // UE 4.21 (5.0) chunks ObjObjects: AllocateObjectPool takes its address and fills in MaxElements and
            // MaxChunks, lea rsi, [rbp+100h]; ...; mov [rsi+18h], ecx (MaxChunks); mov [rsi+10h], eax (CL-4214610
            // 0x178CA71). The fixed array has nothing at +0x18.
            const int32_t ObjObjects = (Members + 3 * static_cast<int32_t>(sizeof(int32_t)) + 1 + 7) & ~7;
            const uintptr_t AllocatePoolEnd = Finder::FunctionEnd(AllocatePool);
            for (uintptr_t Lea = AllocatePoolEnd ? FindInstruction(AllocatePool, AllocatePoolEnd, { 0x48, 0x8D, -1, ObjObjects & 0xFF, (ObjObjects >> 8) & 0xFF, 0x00, 0x00 }) : 0; Lea && !ObjectLayout::Current.bChunkedObjectArray;
                 Lea = FindInstruction(Lea + 1, AllocatePoolEnd, { 0x48, 0x8D, -1, ObjObjects & 0xFF, (ObjObjects >> 8) & 0xFF, 0x00, 0x00 }))
            {
                const uint8_t ModRm = Byte(Lea + 2);
                const uint8_t Address = (ModRm >> 3) & 0x07;
                if ((ModRm & 0xC0) != 0x80 || (ModRm & 0x07) == 0x04 || Address == 0x04)
                {
                    continue;
                }
                for (uintptr_t Store = Lea + 7; Store + 3 <= Lea + 0x40 && Store + 3 <= AllocatePoolEnd; ++Store)
                {
                    if (Byte(Store) == 0x89 && (Byte(Store + 1) & 0xC7) == (0x40 | Address) && Byte(Store + 2) == 0x18 && (Byte(Store - 1) & 0xF1) != 0x41)
                    {
                        ObjectLayout::Current.bChunkedObjectArray = true;
                        break;
                    }
                }
            }
            // 5.21 (CL-4288479) has the members at +0x64, so ObjObjects (+0x78) is close enough to address through
            // the array itself, with no lea: mov [rbp+88h], eax (MaxElements); mov [rbp+90h], ecx (MaxChunks), with
            // rbp the array (0x17DB516). A store at +0x10 and +0x18 from one base is the chunked array's too. 5.30
            // (CL-4305896) has the members at +0x58 and ObjObjects at +0x68, so MaxElements is a byte displacement:
            // mov [rbp+80h], ecx (MaxChunks); mov [rbp+78h], eax (MaxElements) (0x181E506). 5.40 (CL-4352937) has the
            // members at +0x18 and ObjObjects at +0x28, so both are byte displacements: mov [rbp+40h], ecx (MaxChunks);
            // mov [rbp+38h], eax (MaxElements) (0x1839C96). Only UE 4.21 (5.0, CL-4214610) on chunks the array, so only
            // there may MaxChunks be a byte displacement: 4.4.0's members at +0x8 would allow one into its fixed array.
            constexpr uint32_t FirstChunkedChangelist = 4214610;
            const bool bChunksMayBeNear = Packed::Changelist() >= FirstChunkedChangelist;
            for (uintptr_t Store = AllocatePoolEnd && !ObjectLayout::Current.bChunkedObjectArray ? AllocatePool : 0; Store && Store + 6 <= AllocatePoolEnd; ++Store)
            {
                const uint8_t ModRm = Byte(Store + 1);
                const bool bMaxChunksDisp32 = (ModRm & 0xC0) == 0x80 && *reinterpret_cast<const int32_t*>(Store + 2) == ObjObjects + 0x18;
                const bool bMaxChunksDisp8 = bChunksMayBeNear && ObjObjects + 0x18 < 0x80 && (ModRm & 0xC0) == 0x40 && Byte(Store + 2) == ObjObjects + 0x18;
                if (Byte(Store) != 0x89 || !(bMaxChunksDisp32 || bMaxChunksDisp8) || (ModRm & 0x07) == 0x04 || (Byte(Store - 1) & 0xF1) == 0x41)
                {
                    continue;
                }
                for (uintptr_t Other = AllocatePool; Other + 6 <= AllocatePoolEnd; ++Other)
                {
                    const bool bDisp32 = (Byte(Other + 1) & 0xC7) == (0x80 | (ModRm & 0x07)) && *reinterpret_cast<const int32_t*>(Other + 2) == ObjObjects + 0x10;
                    const bool bDisp8 = ObjObjects + 0x10 < 0x80 && (Byte(Other + 1) & 0xC7) == (0x40 | (ModRm & 0x07)) && Byte(Other + 2) == ObjObjects + 0x10;
                    if (Byte(Other) == 0x89 && (bDisp32 || bDisp8) && (Byte(Other - 1) & 0xF1) != 0x41)
                    {
                        ObjectLayout::Current.bChunkedObjectArray = true;
                        break;
                    }
                }
                break;
            }

            // Fortnite's chunks hold 0x10400 items, not UE's 64K: the inlined IndexToObject divides by them (imul eax,
            // edx, 10400h after the 7E07E07Fh reciprocal) right before it loads the chunk table, mov rax,
            // [GUObjectArray+ObjObjects] (CL-4214610 0x287C51B, in KismetSystemLibrary::execIsValidClass). The size
            // most such reads agree on counts. 6.10 (CL-4480234) is back to UE's 64K and divides with a shift, sar
            // ecx, 10h, before mov rax, [GUObjectArray+ObjObjects] (0x2AE7C1, 1131 reads): that counts for 64K, so
            // "0 reads agree" means the reads weren't found rather than that the build shifts.
            const uintptr_t ChunkTable = Array + ObjObjects;
            std::unordered_map<int32_t, int32_t> ChunkSizes;
            for (uintptr_t Load = ObjectLayout::Current.bChunkedObjectArray ? FindInstruction(Finder::TextBegin(), Finder::TextEnd(), { -1, 0x8B, -1, -1, -1, -1, -1 }) : 0; Load; Load = FindInstruction(Load + 1, Finder::TextEnd(), { -1, 0x8B, -1, -1, -1, -1, -1 }))
            {
                if ((Byte(Load) & 0xFB) != 0x48 || (Byte(Load + 2) & 0xC7) != 0x05 || Finder::RelativeTarget(Load, 3, 7) != ChunkTable)
                {
                    continue;
                }
                bool bMultiplied = false;
                for (uintptr_t Multiply = Load - 0x30; Multiply + 6 <= Load; ++Multiply)
                {
                    const int32_t Size = *reinterpret_cast<const int32_t*>(Multiply + 2);
                    if (Byte(Multiply) == 0x69 && (Byte(Multiply + 1) & 0xC0) == 0xC0 && Size >= 0x1000 && Size <= 0x100000)
                    {
                        ++ChunkSizes[Size];
                        bMultiplied = true;
                        break;
                    }
                }
                // sar or shr r32, 10h (C1 /7 or /5 with a register operand, ib 10).
                for (uintptr_t Shift = Load - 0x30; !bMultiplied && Shift + 3 <= Load; ++Shift)
                {
                    if (Byte(Shift) == 0xC1 && ((Byte(Shift + 1) & 0xF8) == 0xF8 || (Byte(Shift + 1) & 0xF8) == 0xE8) && Byte(Shift + 2) == 0x10)
                    {
                        ++ChunkSizes[64 * 1024];
                        break;
                    }
                }
            }
            int32_t ChunkSizeVotes = 0;
            for (const auto& [Size, Votes] : ChunkSizes)
            {
                if (Votes > ChunkSizeVotes)
                {
                    ObjectLayout::Current.ObjectArrayElementsPerChunk = Size;
                    ChunkSizeVotes = Votes;
                }
            }
            if (ObjectLayout::Current.bChunkedObjectArray)
            {
                Log::Info(L"Object layout: ObjObjects (+0x{:X}) is a FChunkedFixedUObjectArray of 0x{:X}-item chunks ({} reads agree)", ObjObjects, ObjectLayout::Current.ObjectArrayElementsPerChunk, ChunkSizeVotes);
            }
        }
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
            uintptr_t Mov = GetNames ? FindInstruction(GetNames, GetNames + 0x20, { 0x48, 0x8B, 0x05 }) : 0;
            if (GetNames && !Mov)
            {
                // 1.10's lookup (FName::GetDisplayNameEntry, 0xC38C30) loads GNames into r9.
                Mov = FindInstruction(GetNames, GetNames + 0x20, { 0x4C, 0x8B, 0x0D });
            }
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

    // "+0x.. (reference i of n)" for a finder's log line, or "not found" with the reference count.
    std::wstring DescribeFromRef(uintptr_t Address, size_t Index, size_t Count)
    {
        return Address ? std::format(L"+0x{:X} (reference {} of {})", Address - Finder::ImageBase(), Index + 1, Count) : std::format(L"not found ({} references)", Count);
    }

    // The StaticConstructObject_Internal a NewObject calls after its inlined empty-name check, whose log text is at Ref:
    // the first call within Window bytes of Ref whose target calls StaticAllocate within AllocateWindow bytes (6.10
    // CL-4480234: FEngineLoop::Init's NewObject<UGameEngine> calls 0x190BA10 0x33 bytes on, which calls
    // StaticAllocateObject at +0xC1). Failing that, the first such call met reading on from Ref through the packer's jumps
    // (Finder::TraceFind), as ResolveLogging reads a UE_LOG the packer split. A moved function's entry is a jump, so a
    // target is also read where it leads (Finder::Unthunk).
    uintptr_t ConstructCalledAfter(uintptr_t Ref, size_t Window, size_t AllocateWindow, uintptr_t StaticAllocate)
    {
        const auto CallsAllocate = [&](uintptr_t Target)
        {
            const uintptr_t Body = Finder::Unthunk(Target);
            return CallWithin(Target, AllocateWindow, StaticAllocate) || (Body != Target && IsInMovedCode(Body) && CallWithin(Body, AllocateWindow, StaticAllocate));
        };
        for (uintptr_t Site = FindRawCall(Ref, Ref + Window); Site; Site = FindRawCall(Site + 5, Ref + Window))
        {
            if (CallsAllocate(Finder::RelativeTarget(Site, 1, 5)))
            {
                return Finder::RelativeTarget(Site, 1, 5);
            }
        }
        const uintptr_t Site = Finder::TraceFind(Ref, 32, [&](uintptr_t Instruction)
        {
            return Byte(Instruction) == 0xE8 && IsInText(Finder::RelativeTarget(Instruction, 1, 5)) && CallsAllocate(Finder::RelativeTarget(Instruction, 1, 5));
        });
        return Site ? Finder::RelativeTarget(Site, 1, 5) : 0;
    }

    void ResolveObjectGlobals()
    {
        // 1.10's MakeUniqueObjectName (0xDE9010, 20 callers) inlines the same check; StaticFindObject (0xDEFCB0) has 218.
        uintptr_t FindObject = 0;
        size_t FindObjectCallers = 0;
        size_t FindObjectRef = 0;
        const std::vector<uintptr_t> FindObjectRefs = Finder::StringRefs(L"Illegal call to StaticFindObject() while serializing object data!");
        for (size_t Index = 0; Index < FindObjectRefs.size(); ++Index)
        {
            const uintptr_t Function = Finder::FunctionStart(FindObjectRefs[Index]);
            if (!FindObject)
            {
                FindObject = Function;
                FindObjectRef = Index;
            }
            else if (Function && Function != FindObject)
            {
                FindObjectCallers = FindObjectCallers ? FindObjectCallers : Finder::CallsTo(FindObject, false).size();
                const size_t Callers = Finder::CallsTo(Function, false).size();
                if (Callers > FindObjectCallers)
                {
                    FindObject = Function;
                    FindObjectCallers = Callers;
                    FindObjectRef = Index;
                }
            }
        }
        Require(L"StaticFindObject", Natives::StaticFindObject, FindObject);
        size_t LoadObjectRef = 0;
        size_t LoadObjectRefs = 0;
        const uintptr_t LoadObject = FunctionOfFirstRef(L"Calling StaticLoadObject during PostLoad may result in hitches during streaming.", LoadObjectRef, LoadObjectRefs);
        Require(L"StaticLoadObject", Natives::StaticLoadObject, LoadObject);

        // StaticAllocateObject isn't a native Core calls, only how StaticConstructObject_Internal is told apart.
        size_t AllocateRef = 0;
        size_t AllocateRefs = 0;
        const uintptr_t StaticAllocate = FunctionOfFirstRef(L"Objects have the same fully qualified name but different paths.\n\tNew Object: %s %s.%s\n\tExisting Object: %s", AllocateRef, AllocateRefs);
        uintptr_t Construct = 0;
        std::wstring ConstructFrom;
        const std::vector<uintptr_t> Refs = Finder::StringRefs(L"NewObject with empty name can't be used to create default subobjects (inside of UObject derived class constructor) as it produces inconsistent object names. Use ObjectInitializer.CreateDefaultSuobject<> instead.");
        const uintptr_t Ref = First(Refs);
        if (StaticAllocate && Ref)
        {
            for (uintptr_t Site = FindRawCall(Ref, Ref + 0x80); Site && !Construct; Site = FindRawCall(Site + 5, Ref + 0x80))
            {
                const uintptr_t Target = Finder::RelativeTarget(Site, 1, 5);
                if (CallWithin(Target, 0x200, StaticAllocate))
                {
                    Construct = Target;
                    ConstructFrom = std::format(L"called 0x{:X} bytes after reference 1 of {}", Site - Ref, Refs.size());
                }
            }
        }

        // The check is inlined into every NewObject (915 references on 6.10 CL-4480234, 904 on 6.02), and the first is
        // FEngineLoop::Init's NewObject<UGameEngine>, a function the packer moves and splits into pieces joined by jumps
        // at places that differ from exe to exe. 6.10 CL-4464155 split it so that no call within 0x80 bytes of that
        // reference reached a function calling StaticAllocateObject, and with it missing neither the client's
        // redirect nor the gameserver started (Core-Backend #340). So when the first reference fails, the first
        // MaxConstructRefs references each name the function they call (with the same check, then slightly wider
        // windows), and the one most of them name is taken. A single caller of StaticAllocateObject other than it can
        // pass the check too (6.02 0x18A0A00, reached from one reference of 904), so it has to win at least two of them
        // and more than half.
        constexpr size_t MaxConstructRefs = 64;
        struct FConstructWindows
        {
            size_t Call;
            size_t Allocate;
        };
        for (const FConstructWindows Windows : { FConstructWindows{ 0x80, 0x200 }, FConstructWindows{ 0x100, 0x300 } })
        {
            if (Construct || !StaticAllocate || Refs.size() < 2)
            {
                break;
            }
            std::unordered_map<uintptr_t, int32_t> Votes;
            std::unordered_map<uintptr_t, size_t> FirstVoter;
            int32_t Cast = 0;
            uintptr_t Best = 0;
            int32_t BestVotes = 0;
            const size_t Read = std::min(Refs.size(), MaxConstructRefs);
            for (size_t Index = 0; Index < Read; ++Index)
            {
                const uintptr_t Target = ConstructCalledAfter(Refs[Index], Windows.Call, Windows.Allocate, StaticAllocate);
                if (!Target)
                {
                    continue;
                }
                ++Cast;
                FirstVoter.emplace(Target, Index);
                if (++Votes[Target] > BestVotes)
                {
                    Best = Target;
                    BestVotes = Votes[Target];
                }
            }
            if (Best && BestVotes >= 2 && BestVotes * 2 > Cast)
            {
                Construct = Best;
                ConstructFrom = std::format(L"reference 1 of {} reaches no function that calls StaticAllocateObject within 0x80 bytes; {} of references 1 to {} call this one within 0x{:X} bytes, first reference {}",
                    Refs.size(), BestVotes, Read, Windows.Call, FirstVoter[Best] + 1);
            }
            else
            {
                Log::Warning(L"Finder: StaticConstructObject_Internal: references 1 to {} of {} name no function most of them call within 0x{:X} bytes ({} of {} for +0x{:X})",
                    Read, Refs.size(), Windows.Call, BestVotes, Cast, Best ? Best - Finder::ImageBase() : 0);
            }
        }
        Require(L"StaticConstructObject_Internal", Natives::StaticConstructObject_Internal, Construct);

        Log::Info(L"Finder: object globals: StaticFindObject {}, StaticLoadObject {}, StaticAllocateObject {}, StaticConstructObject_Internal {}",
            DescribeFromRef(FindObject, FindObjectRef, FindObjectRefs.size()), DescribeFromRef(LoadObject, LoadObjectRef, LoadObjectRefs), DescribeFromRef(StaticAllocate, AllocateRef, AllocateRefs),
            Construct ? std::format(L"+0x{:X} ({})", Construct - Finder::ImageBase(), ConstructFrom) : std::format(L"not found ({} references)", Refs.size()));
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

    // FEngineLoop::PreInit's run of `GIsClient = GIsServer = true` and the flag after them. 1.1 to 4.1 write one
    // `mov byte [rip+X], 1` each; 4.2 holds the 1 in a register for the whole function and writes
    // `mov [rip+X], r13b`, which is the same three assignments with the immediate hoisted out, so a run of byte
    // stores through one register stands in for them. Three of them is the run: the window is the code right in
    // front of one string, and a fourth store would mean this is not that run and the mapping below would be wrong.
    std::vector<std::pair<uintptr_t, uintptr_t>> FlagStores(uintptr_t Begin, uintptr_t End)
    {
        std::vector<std::pair<uintptr_t, uintptr_t>> Immediates = MovStores(Begin, End, { 0xC6, 0x05 }, 2, 7);
        Immediates.erase(std::remove_if(Immediates.begin(), Immediates.end(), [](const auto& Store) { return Byte(Store.first + 6) != 0x01; }), Immediates.end());
        if (!Immediates.empty())
        {
            return Immediates;
        }

        // mov byte [rip+X], r8b to r15b: REX.R, 88 /r, and a modrm naming rip with the register in its reg field.
        for (uint8_t Register = 0; Register < 8; ++Register)
        {
            std::vector<std::pair<uintptr_t, uintptr_t>> Through = MovStores(Begin, End, { 0x44, 0x88, 0x05 | Register << 3 }, 3, 7);
            if (Through.size() == 3)
            {
                return Through;
            }
        }
        return Immediates;
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

        std::vector<std::pair<uintptr_t, uintptr_t>> Flags = FlagStores(Ref - 0x40, Ref);
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

    // Whether any code loads the global (mov r64, [rip+Global]).
    bool IsReadByCode(uintptr_t Global)
    {
        for (const auto& [Begin, End] : Finder::ExecutableSections())
        {
            for (uintptr_t Load = Begin; Load + 7 <= End; ++Load)
            {
                if ((Byte(Load) & 0xFB) == 0x48 && Byte(Load + 1) == 0x8B && (Byte(Load + 2) & 0xC7) == 0x05 && Finder::RelativeTarget(Load, 3, 7) == Global)
                {
                    return true;
                }
            }
        }
        return false;
    }

    void ResolveEngineReadiness()
    {
        uintptr_t Engine = 0;
        std::vector<uintptr_t> InitOnes;
        const uintptr_t Init = FunctionOf(L"Failed to load UnrealEd Engine class '%s'.");
        const uintptr_t InitEnd = Finder::FunctionEnd(Init);
        if (Init && InitEnd)
        {
            const std::vector<std::pair<uintptr_t, uintptr_t>> Stores = MovStores(Init, InitEnd, { 0x48, 0x89, 0x05 }, 3, 7);
            Engine = Stores.empty() ? 0 : Stores.front().second;

            for (const auto& Store : MovStores(Init, InitEnd, { 0xC6, 0x05 }, 2, 7))
            {
                if (Byte(Store.first + 6) == 0x01)
                {
                    InitOnes.push_back(Store.second);
                }
            }
        }
        if (Engine && Finder::HasMovedCode() && !IsReadByCode(Engine))
        {
            // 4.4.0 (CL-4117433) leaves a piece of FEngineLoop::Init that names the class in .text (0x537740), but its
            // store is to a global no code reads (0x5379AE: mov [rip+0x4F40310], rax), while GEngine is read in about
            // 1,500 places. That piece is not the one that makes the engine, so neither its store nor its flags count.
            Engine = 0;
            InitOnes.clear();
        }
        if (!Engine)
        {
            // 1.9.1's FEngineLoop::Init sits outside .text, in pieces with no .pdata entry, so there is no function to
            // read the store out of. 2.3 leaves the piece that names the class in .text, which does have one, but the
            // store is in a piece of its own, so the function is found and holds no store. GEngine is also the global
            // the PIE online-subsystem check loads first.
            const uintptr_t Check = FunctionOf(L"Failed to determine if OSS is server in PIE, OSS requests will fail");
            const uintptr_t Load = Check ? FindInstruction(Check, Check + 0x10, { 0x48, 0x8B, 0x1D }) : 0;
            Engine = Load ? Finder::RelativeTarget(Load, 3, 7) : 0;
        }
        if (!Engine)
        {
            // 4.2 drops that check, so read it where FEngineLoop::Init uses it: the log "Initializing Game Engine..."
            // is the line before GEngine->Init(this), which loads the engine into rcx to reach its vtable. Following
            // jumps, since Init is in the moved code and split into pieces.
            const uintptr_t Ref = First(Finder::StringRefs(L"Initializing Game Engine..."));
            const uintptr_t Load = Finder::TraceFind(Ref, 16, [](uintptr_t Instruction)
            {
                return Byte(Instruction) == 0x48 && Byte(Instruction + 1) == 0x8B && Byte(Instruction + 2) == 0x0D;
            });
            Engine = Load ? Finder::RelativeTarget(Load, 3, 7) : 0;
        }
        RequireAddress(L"GEngine", reinterpret_cast<uintptr_t&>(Natives::GEngine), Engine);

        const uintptr_t HandleError = FunctionOf(L"HandleError re-entered.");
        const uintptr_t HandleErrorEnd = Finder::FunctionEnd(HandleError);
        std::vector<uintptr_t> ErrorZeros;
        if (HandleError && HandleErrorEnd)
        {
            for (const auto& Store : MovStores(HandleError, HandleErrorEnd, { 0xC6, 0x05 }, 2, 7))
            {
                if (Byte(Store.first + 6) == 0x00)
                {
                    ErrorZeros.push_back(Store.second);
                }
            }
        }

        uintptr_t Running = 0;
        for (const uintptr_t Zero : ErrorZeros)
        {
            if (std::find(InitOnes.begin(), InitOnes.end(), Zero) != InitOnes.end())
            {
                Running = Zero;
            }
        }
        if (!Running && InitOnes.empty())
        {
            // Without FEngineLoop::Init: GIsRunning is the flag the pure virtual call handler tests just before it names
            // "PureVirtualFunctionCalledWhileRunningApp", and one HandleError clears.
            const uintptr_t Ref = First(Finder::StringRefs(L"PureVirtualFunctionCalledWhileRunningApp"));
            const uintptr_t Test = FindBackward(Ref, 0x20, { 0x80, 0x3D, -1, -1, -1, -1, 0x00 });
            const uintptr_t Flag = Test ? Finder::RelativeTarget(Test, 2, 7) : 0;
            if (Flag && std::find(ErrorZeros.begin(), ErrorZeros.end(), Flag) != ErrorZeros.end())
            {
                Running = Flag;
            }
        }
        RequireAddress(L"GIsRunning", reinterpret_cast<uintptr_t&>(Natives::GIsRunning), Running);
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

            // UGameEngine::GameViewportWindow (a TWeakPtr<SWindow>), from Tick's first frame: bFirstTime = false;
            // FPlatformSplash::Hide(); if (GameViewportWindow.IsValid()) { ShowWindow; RegisterGameViewport(
            // GameViewportWidget) }, which tests the weak pointer's object, then its reference controller:
            // cmp qword ptr [this+Window], 0; jz; mov rax, [this+Window+8] (4.5 CL-4166199 0x217D6E5, +0xBF0). The
            // widget is the TSharedPtr<SViewport> after SceneViewport (GameEngine.h), Window+0x20, read for the
            // RegisterGameViewport call further on (+0xC10).
            ptrdiff_t ViewportWindow = 0;
            int32_t ViewportWindowSites = 0;
            for (uintptr_t Compare = FindInstruction(Tick, TickEnd, { -1, 0x83, -1, -1, -1, 0x00, 0x00, 0x00 }); Compare; Compare = FindInstruction(Compare + 1, TickEnd, { -1, 0x83, -1, -1, -1, 0x00, 0x00, 0x00 }))
            {
                const uint8_t Rex = Byte(Compare);
                const uint8_t ModRm = Byte(Compare + 2);
                if ((Rex != 0x48 && Rex != 0x49) || (ModRm & 0xF8) != 0xB8 || (ModRm & 0x07) == 0x04)
                {
                    continue;
                }
                const auto BaseOf = [](uint8_t Prefix, uint8_t Operand) { return static_cast<uint8_t>(((Prefix & 0x01) << 3) | (Operand & 0x07)); };
                const uint8_t Base = BaseOf(Rex, ModRm);
                const int32_t Window = *reinterpret_cast<const int32_t*>(Compare + 3);
                const uintptr_t Jcc = Compare + 8;
                const uintptr_t AfterJcc = Byte(Jcc) == 0x74 ? Jcc + 2 : Byte(Jcc) == 0x0F && Byte(Jcc + 1) == 0x84 ? Jcc + 6 : 0;
                const auto LoadsField = [&](uintptr_t From, uintptr_t To, int32_t Field)
                {
                    for (uintptr_t Load = From; Load + 7 <= To; ++Load)
                    {
                        if ((Byte(Load) & 0xFA) == 0x48 && Byte(Load + 1) == 0x8B && (Byte(Load + 2) & 0xC0) == 0x80 && (Byte(Load + 2) & 0x07) != 0x04
                            && BaseOf(Byte(Load), Byte(Load + 2)) == Base && *reinterpret_cast<const int32_t*>(Load + 3) == Field)
                        {
                            return true;
                        }
                    }
                    return false;
                };
                if (Window > 0 && AfterJcc && LoadsField(AfterJcc, AfterJcc + 0x10, Window + 8) && LoadsField(AfterJcc, std::min(AfterJcc + 0x100, TickEnd), Window + 0x20))
                {
                    ViewportWindow = Window;
                    ++ViewportWindowSites;
                }
            }
            // 5.0 (UE 4.21) holds the engine in r12 and compares with the register it cleared bFirstTime with:
            // cmp [r12+0C28h], rdi; jz (CL-4214610 0x23C685B), then mov rbx, [r12+0C30h] and, further on, [r12+0C48h].
            // r12 as a base takes a SIB byte (24h); other bases don't.
            struct FMemoryOperand
            {
                uint8_t Base = 0;
                int32_t Displacement = 0;
                uintptr_t Next = 0;
            };
            // REX.W Opcode ModRm [SIB 24h] disp32, the ModRm a [base+disp32] (mod 10) with no index.
            const auto DecodeMemory = [](uintptr_t At, uint8_t Opcode, FMemoryOperand& Out)
            {
                const uint8_t Rex = Byte(At);
                const uint8_t ModRm = Byte(At + 2);
                if ((Rex & 0xF8) != 0x48 || Byte(At + 1) != Opcode || (ModRm & 0xC0) != 0x80)
                {
                    return false;
                }
                const bool bSib = (ModRm & 0x07) == 0x04;
                if (bSib && Byte(At + 3) != 0x24)
                {
                    return false;
                }
                Out.Base = static_cast<uint8_t>(((Rex & 0x01) << 3) | (ModRm & 0x07));
                Out.Displacement = *reinterpret_cast<const int32_t*>(At + (bSib ? 4 : 3));
                Out.Next = At + (bSib ? 8 : 7);
                return true;
            };
            for (uintptr_t Compare = ViewportWindowSites == 0 ? Tick : 0; Compare && Compare + 8 <= TickEnd; ++Compare)
            {
                FMemoryOperand Operand;
                if (!DecodeMemory(Compare, 0x39, Operand))
                {
                    continue;
                }
                const int32_t Window = Operand.Displacement;
                const uintptr_t Jcc = Operand.Next;
                const uintptr_t AfterJcc = Byte(Jcc) == 0x74 ? Jcc + 2 : Byte(Jcc) == 0x0F && Byte(Jcc + 1) == 0x84 ? Jcc + 6 : 0;
                const auto LoadsField = [&](uintptr_t From, uintptr_t To, int32_t Field)
                {
                    for (uintptr_t Load = From; Load + 8 <= To; ++Load)
                    {
                        FMemoryOperand Loaded;
                        if (DecodeMemory(Load, 0x8B, Loaded) && Loaded.Base == Operand.Base && Loaded.Displacement == Field)
                        {
                            return true;
                        }
                    }
                    return false;
                };
                if (Window > 0 && AfterJcc && LoadsField(AfterJcc, AfterJcc + 0x10, Window + 8) && LoadsField(AfterJcc, std::min(AfterJcc + 0x100, TickEnd), Window + 0x20))
                {
                    ViewportWindow = Window;
                    ++ViewportWindowSites;
                }
            }
            if (ViewportWindowSites == 1)
            {
                Natives::UGameEngine_GameViewportWindow = ViewportWindow;
                Log::Info(L"Finder: UGameEngine::GameViewportWindow +0x{:X}, GameViewportWidget +0x{:X}", ViewportWindow, ViewportWindow + 0x20);
            }
            else
            {
                // Only a build whose PreLoadScreen hands the engine a window (4.5+) needs it (UGameEngine::ReleasePreLoadScreenWindow).
                Log::Info(L"Finder: UGameEngine::GameViewportWindow not found ({} candidates), only a PreLoadScreen window needs it", ViewportWindowSites);
            }
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

    // CallBeforeJccPastRef for moved code (Finder::HasMovedCode), where a function's pieces sit apart, joined by jumps,
    // and its calls are call; nop; jmp. Of the calls whose result is tested (test al, al; jnz) with Reference on the
    // side that isn't taken, the one that runs into it in the fewest instructions.
    uintptr_t MovedCallBeforeJccPastRef(uintptr_t Reference)
    {
        uintptr_t Found = 0;
        int32_t FoundSteps = -1;
        int32_t Ties = 0;
        for (const auto& [Begin, End] : Finder::ExecutableSections())
        {
            for (uintptr_t Call = Begin; Call + 7 <= End; ++Call)
            {
                if (Byte(Call) != 0xE8 || Byte(Call + 5) != 0x90 || (Byte(Call + 6) != 0xE9 && Byte(Call + 6) != 0xEB) || !IsInText(Finder::RelativeTarget(Call, 1, 5)))
                {
                    continue;
                }
                const uintptr_t Test = Finder::Unthunk(Call + 6);
                if (Byte(Test) != 0x84 || Byte(Test + 1) != 0xC0)
                {
                    continue;
                }
                const uintptr_t NotTaken = Byte(Test + 2) == 0x75 ? Test + 4 : Byte(Test + 2) == 0x0F && Byte(Test + 3) == 0x85 ? Test + 8 : 0;
                const int32_t Steps = NotTaken ? Finder::RunsInto(NotTaken, Reference, 512) : -1;
                if (Steps < 0 || (FoundSteps >= 0 && Steps > FoundSteps))
                {
                    continue;
                }
                Ties = Steps == FoundSteps ? Ties + 1 : 1;
                Found = Call;
                FoundSteps = Steps;
            }
        }
        return Ties == 1 ? Found : 0;
    }

    // On a packed exe (Finder::HasMovedCode) the "LoadMap: failed to Listen(%s)" reference and LoadMap's world/url
    // setup for the Listen call sit in moved code that can still be ciphertext when Core resolves, so the log anchor
    // finds nothing. The call instruction itself is in the first .text: UWorld::Listen is stripped to xor al, al; ret,
    // so LoadMap's call to it is a call; nop; jmp whose target is that stub, reached right after the
    // FURL::HasOption("Listen") test (the previous call; nop; jmp, whose result is tested with test al, al; jcc). That
    // pair is unique in the first .text, so it names the same call Core hooks on 1.9 without reading any moved code.
    uintptr_t PackedListenCall(int32_t& OutCount)
    {
        const uintptr_t Begin = Finder::TextBegin();
        const uintptr_t End = Finder::TextEnd();
        uintptr_t Found = 0;
        int32_t Count = 0;
        for (uintptr_t Site = Begin; Site + 8 <= End; ++Site)
        {
            if (Byte(Site) != 0xE8 || Byte(Site + 5) != 0x90 || (Byte(Site + 6) != 0xE9 && Byte(Site + 6) != 0xEB))
            {
                continue;
            }
            const uintptr_t Target = Finder::RelativeTarget(Site, 1, 5);
            if (!IsInText(Target) || !Finder::BodyIs(Target, { 0x32, 0xC0, 0xC3 }))
            {
                continue;
            }
            uintptr_t Previous = 0;
            for (uintptr_t Scan = Site - 1; Scan + 0x40 >= Site && !Previous; --Scan)
            {
                if (Byte(Scan) == 0xE8 && Byte(Scan + 5) == 0x90 && (Byte(Scan + 6) == 0xE9 || Byte(Scan + 6) == 0xEB))
                {
                    Previous = Scan;
                }
            }
            if (!Previous)
            {
                continue;
            }
            // On 2.2 the test is in the second section, where its jcc is rewritten into a cmov of the address to go to.
            // On 2.5 the jmp lands on another jmp in the first .text that leads there, so follow the whole chain.
            const uintptr_t Jump = Previous + 6;
            const uintptr_t Test = Finder::Unthunk(Jump);
            const bool bJcc = IsInText(Test) && (Byte(Test + 2) == 0x74 || Byte(Test + 2) == 0x75 || (Byte(Test + 2) == 0x0F && (Byte(Test + 3) == 0x84 || Byte(Test + 3) == 0x85)));
            // On 3.0 the test is in the first .text (0x59EF29) and its branch is a jmp into the second section, where
            // a cmov picks the address to go to.
            const bool bMovedBranch = IsInText(Test) && (Byte(Test + 2) == 0xE9 || Byte(Test + 2) == 0xEB) && IsInMovedCode(Finder::Unthunk(Test + 2));
            const bool bTest = (bJcc || bMovedBranch || IsInMovedCode(Test)) && Byte(Test) == 0x84 && Byte(Test + 1) == 0xC0;
            if (bTest)
            {
                Found = Site;
                ++Count;
            }
        }
        OutCount = Count;
        return Count == 1 ? Found : 0;
    }

    // PackedListenCall's fallback, for 4.4.0/4.4.1 (CL-4117433, CL-4127312), where its HasOption pairing did not hold
    // on 4.4.1. The "LoadMap: failed to Listen(%s)" reference's moved piece jumps back to the UE_LOG call in LoadMap's
    // first-.text code (4.4.0: ref 0x554008F, log call 0x24CEFAA), and the Listen call (0x24CEF4B) is the only call
    // to a stripped xor al, al; ret function within 0x200 bytes of it, on 4.4.0 and 4.4.2 alike. 4.5 puts the moved
    // piece in the first .text itself (CL-4166199: ref 0x10F6B, jmp to the log call 0x24F777A, Listen call 0x24F771B;
    // CL-4159770: ref 0xC3AC, log call 0x24F85AA, Listen call 0x24F854B), so the trace looks past the reference's own
    // piece. 4.5's HasOption test (0x3B77F4) has a store between its test al, al and the jcc, which PackedListenCall
    // doesn't take.
    uintptr_t ListenCallNearLog(uintptr_t ListenLog)
    {
        const uintptr_t LogSite = ListenLog ? Finder::TraceFind(ListenLog, 16, [ListenLog](uintptr_t Instruction)
        {
            return IsInText(Instruction) && (Instruction < ListenLog || Instruction >= ListenLog + 0x40);
        }) : 0;
        if (!LogSite)
        {
            Log::Error(L"Finder: no first-.text code after the LoadMap Listen log reference (+0x{:X})", ListenLog ? ListenLog - Finder::ImageBase() : 0);
            return 0;
        }
        constexpr uintptr_t Window = 0x200;
        const uintptr_t Begin = std::max(Finder::TextBegin(), LogSite - Window);
        const uintptr_t End = std::min(Finder::TextEnd() - 5, LogSite + Window);
        uintptr_t Found = 0;
        int32_t Count = 0;
        for (uintptr_t Site = Begin; Site < End; ++Site)
        {
            if (Byte(Site) != 0xE8)
            {
                continue;
            }
            const uintptr_t Target = Finder::RelativeTarget(Site, 1, 5);
            if (IsInText(Target) && Finder::BodyIs(Target, { 0x32, 0xC0, 0xC3 }))
            {
                Found = Site;
                ++Count;
            }
        }
        if (Count != 1)
        {
            Log::Error(L"Finder: expected 1 stripped call near the LoadMap Listen log at +0x{:X}, found {}", LogSite - Finder::ImageBase(), Count);
        }
        return Count == 1 ? Found : 0;
    }

    constexpr int32_t WorldNetDriver = 0x38;

    // UWorld::DemoNetDriver: +0xB8 up to 2.1, +0x108 on 2.2, whose UWorld adds StreamingLevelsToConsider (a TSet) before
    // it. Read from UGameInstance::StartRecordingReplay's store of the driver it found (ResolveEngineNet).
    int32_t WorldDemoNetDriver = 0;

    uint8_t BaseRegister(uint8_t Rex, uint8_t ModRm)
    {
        return static_cast<uint8_t>(((Rex & 0x01) << 3) | (ModRm & 0x07));
    }

    bool FindInlinedNetMode(uintptr_t Call, uintptr_t GetNetMode, Sites::FInlinedNetMode& Out)
    {
        uintptr_t DemoLoad = 0;
        for (uintptr_t Scan = Call - 0x40; Scan + 7 <= Call; ++Scan)
        {
            if ((Byte(Scan) & 0xF8) == 0x48 && Byte(Scan + 1) == 0x8B && (Byte(Scan + 2) & 0xC0) == 0x80 && (Byte(Scan + 2) & 0x07) != 0x04 && WorldDemoNetDriver && *reinterpret_cast<const int32_t*>(Scan + 3) == WorldDemoNetDriver)
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
        Require(L"UGameInstance::StartGameInstance", Natives::UGameInstance_StartGameInstance, FunctionOf(L"Failed to enter %s: %s. Please check the log for errors."));
        Require(L"GetSourceIniHierarchyFilenames", Natives::GetSourceIniHierarchyFilenames, FunctionOf(L"%sNotForLicensees/Default%s.ini"));

        // On 1.9.1 UEngine::LoadMap is moved code, its log reference too: the registers for its Listen call are set in
        // other pieces, so there only the call's target is checked.
        // On packed builds with moved code (2.4, 2.4.2) the reference can still be ciphertext here, so the string has none
        // and PackedListenCall finds the call; that is expected there, not worth a warning.
        const uintptr_t ListenLog = Finder::HasMovedCode() ? First(Finder::StringRefs(L"LoadMap: failed to Listen(%s)")) : OnlyRef(L"LoadMap: failed to Listen(%s)");
        const bool bMovedListen = ListenLog && !IsInText(ListenLog);
        uintptr_t ListenCall = ListenLog ? (bMovedListen ? MovedCallBeforeJccPastRef(ListenLog) : CallBeforeJccPastRef(ListenLog, 0x100)) : 0;
        bool bListen = ListenCall && (bMovedListen || IsListenCall(ListenCall));
        if (!bListen && Finder::HasMovedCode())
        {
            // The log reference and the call's register setup can be ciphertext when Core resolves; find the call in
            // the first .text instead.
            int32_t PackedCount = 0;
            uintptr_t Packed = PackedListenCall(PackedCount);
            if (!Packed)
            {
                Packed = ListenCallNearLog(ListenLog);
                if (Packed)
                {
                    Log::Info(L"Finder: LoadMap Listen call at +0x{:X}, the stripped call next to its log ({} HasOption-paired calls)", Packed - Finder::ImageBase(), PackedCount);
                }
                else
                {
                    Log::Error(L"Finder: expected 1 first-.text LoadMap Listen call, found {}", PackedCount);
                }
            }
            if (Packed)
            {
                ListenCall = Packed;
                bListen = true;
            }
        }
        if (bListen)
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

                // CurrentWorld->DemoNetDriver = Cast(FindNamedNetDriver(...)), then CurrentWorld->DemoNetDriver->SetWorld:
                // mov [world+0xB8], reg up to 2.1; 2.2 is known by the load that follows, mov rcx, [world+0x108].
                uintptr_t DemoStore = 0;
                for (uintptr_t Store = FindNamed ? FindInstruction(FindCall, FindCall + 0x40, { -1, 0x89 }) : 0; Store && !DemoStore; Store = FindInstruction(Store + 1, FindCall + 0x40, { -1, 0x89 }))
                {
                    const int32_t Offset = *reinterpret_cast<const int32_t*>(Store + 3);
                    const bool bStore = (Byte(Store) & 0xF8) == 0x48 && (Byte(Store + 2) & 0xC0) == 0x80 && (Byte(Store + 2) & 0x07) != 0x04;
                    const bool bLoad = (Byte(Store + 7) & 0xF8) == 0x48 && Byte(Store + 8) == 0x8B && (Byte(Store + 9) & 0xF8) == 0x88 && (Byte(Store + 9) & 0x07) == (Byte(Store + 2) & 0x07) && *reinterpret_cast<const int32_t*>(Store + 10) == Offset;
                    if (bStore && (Offset == 0xB8 || (bLoad && Offset > WorldNetDriver && Offset < 0x200)))
                    {
                        DemoStore = Store;
                        WorldDemoNetDriver = Offset;
                    }
                }
                // 5.0 (UE 4.21) inlines a longer Cast, so the store is further on (CL-4214610 0x23C4BD1, 0x46 past the
                // call), and SetWorld is called on the stored register straight away: mov [rdi+108h], rbx; mov rdx, rdi;
                // mov rcx, rbx; call SetWorld.
                for (uintptr_t Store = FindNamed && !DemoStore ? FindInstruction(FindCall, FindCall + 0x60, { -1, 0x89 }) : 0; Store && !DemoStore; Store = FindInstruction(Store + 1, FindCall + 0x60, { -1, 0x89 }))
                {
                    const int32_t Offset = *reinterpret_cast<const int32_t*>(Store + 3);
                    if ((Byte(Store) & 0xF8) != 0x48 || (Byte(Store + 2) & 0xC0) != 0x80 || (Byte(Store + 2) & 0x07) == 0x04 || Offset <= WorldNetDriver || Offset >= 0x200)
                    {
                        continue;
                    }
                    const uint8_t Stored = static_cast<uint8_t>(((Byte(Store) & 0x04) << 1) | ((Byte(Store + 2) >> 3) & 0x07));
                    const uintptr_t Call = FindRawCall(Store + 7, Store + 0x14);
                    for (uintptr_t Move = Store + 7; Call && Move + 3 <= Call && !DemoStore; ++Move)
                    {
                        // mov rcx, Stored: REX.W (REX.B for r8-r15) 8B, ModRm 11 001 rm.
                        if (Byte(Move) == (Stored >= 8 ? 0x49 : 0x48) && Byte(Move + 1) == 0x8B && Byte(Move + 2) == (0xC8 | (Stored & 0x07)))
                        {
                            DemoStore = Store;
                            WorldDemoNetDriver = Offset;
                        }
                    }
                }
                if (DemoStore)
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
                // On 1.9.1 (Finder::HasMovedCode) junk replaced the function before it, which was moved: no int3 padding.
                if ((Byte(Start - 1) == 0xCC || Finder::HasMovedCode()) && Byte(Start) == 0x48 && Byte(Start + 1) == 0x83 && Byte(Start + 2) == 0x79 && Byte(Start + 4) == 0x00)
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
        // 5.0 (UE 4.21) inlines AttemptDeriveFromURL into InternalGetNetMode, the function with the "listen" strings
        // (CL-4214610 0x27C27C0): push rbx; sub rsp, 90h; cmp qword ptr [rcx+38h], 0 (NetDriver); ...; mov rcx,
        // [rcx+108h] (DemoNetDriver); ...; jmp UNetDriver::GetNetMode; then the URL's derivation. Its 84 callers are
        // GetNetMode's calls, with no inlined copies to patch. The URL derivation is reached through the original
        // function with both drivers null (UWorld::Hook keeps its trampoline as AttemptDeriveFromURL).
        if (!InternalGetNetMode && FindInstruction(Derive, Derive + 0x10, { 0x48, 0x83, 0x79, WorldNetDriver, 0x00 }))
        {
            const uintptr_t DeriveEnd = Finder::FunctionEnd(Derive);
            const uintptr_t Jump = DeriveEnd > Derive ? FindInstruction(Derive, (std::min)(DeriveEnd, Derive + 0x60), { 0xE9 }) : 0;
            const uintptr_t Target = Jump ? Finder::RelativeTarget(Jump, 1, 5) : 0;
            if (IsInText(Target))
            {
                InternalGetNetMode = Derive;
                GetNetMode = Target;
                Natives::bInternalGetNetModeDerivesFromURL = true;
                Calls.clear();
                Log::Info(L"Finder: UWorld::InternalGetNetMode derives the URL's net mode itself, GetNetMode's calls need no patching");
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
        if (!Natives::bInternalGetNetModeDerivesFromURL && (Calls.size() != std::size(Sites::InlinedNetMode) || Found != static_cast<int32_t>(Calls.size())))
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
        if (!Broadcast && Natives::NumDelegateInlineBytes == 0)
        {
            // 5.0 (UE 4.21) inlines the broadcast over the heap invocation list (CL-4214610 0x2783510): lea r14,
            // [r15+930h] (NetworkFailureEvent); inc dword ptr [r14+14h] (its lock count); ...; mov r9d,
            // PendingConnectionFailure; ...; call [rax+40h] (ExecuteIfSafe). Core broadcasts it the same way itself
            // (TMulticastDelegate::Broadcast), so only the event's offset is needed.
            for (uintptr_t Type = FindInstruction(Travel, TravelEnd, { 0x41, 0xB9, ENetworkFailure::PendingConnectionFailure, 0x00, 0x00, 0x00 }); Type && Natives::UEngine_NetworkFailureEvent < 0; Type = FindInstruction(Type + 1, TravelEnd, { 0x41, 0xB9, ENetworkFailure::PendingConnectionFailure, 0x00, 0x00, 0x00 }))
            {
                for (uintptr_t Lea = Type - 0x80; Lea + 11 <= Type; ++Lea)
                {
                    // lea reg, [base+disp32] (REX.W, mod 10, no SIB), then inc dword ptr [reg+14h].
                    const uint8_t Rex = Byte(Lea);
                    const uint8_t ModRm = Byte(Lea + 2);
                    if ((Rex & 0xF8) != 0x48 || Byte(Lea + 1) != 0x8D || (ModRm & 0xC0) != 0x80 || (ModRm & 0x07) == 0x04)
                    {
                        continue;
                    }
                    const uint8_t Destination = static_cast<uint8_t>(((Rex & 0x04) << 1) | ((ModRm >> 3) & 0x07));
                    const uintptr_t Increment = Lea + 7;
                    const bool bIncrement = Byte(Increment) == (Destination >= 8 ? 0x41 : 0xFF) && Byte(Increment + (Destination >= 8 ? 1 : 0)) == 0xFF;
                    const uintptr_t IncrementModRm = Increment + (Destination >= 8 ? 2 : 1);
                    if (bIncrement && Byte(IncrementModRm) == (0x40 | (Destination & 0x07)) && Byte(IncrementModRm + 1) == 0x14)
                    {
                        Natives::UEngine_NetworkFailureEvent = *reinterpret_cast<const int32_t*>(Lea + 3);
                        Log::Info(L"Finder: FOnNetworkFailure::Broadcast is inlined, Core broadcasts UEngine::NetworkFailureEvent (+0x{:X}) itself", Natives::UEngine_NetworkFailureEvent);
                        break;
                    }
                }
            }
            if (Natives::UEngine_NetworkFailureEvent > 0)
            {
                return;
            }
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
        std::vector<std::pair<uintptr_t, uintptr_t>> Candidates;
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
                Candidates.emplace_back(Target, TargetFirst);
                ++Matches;
            }
        }

        const uintptr_t PrefixRef = OnlyRef(L"Failed to load game mode '%s' specified by prefixed map name %s.");
        const uintptr_t CreateGameMode = Finder::FunctionStart(PrefixRef);
        const std::vector<std::pair<uintptr_t, uintptr_t>> CreateCalls = CreateGameMode ? Finder::DirectCalls(PrefixRef, Finder::FunctionEnd(CreateGameMode)) : std::vector<std::pair<uintptr_t, uintptr_t>>();
        // GetGlobalDefaultGameMode: the one function CreateGameModeForURL calls whose first call is the settings' StaticClass.
        const auto FindGlobalDefaultGameMode = [&](uintptr_t SettingsClass)
        {
            uintptr_t Found = 0;
            int32_t FoundMatches = 0;
            for (const auto& [Site, Target] : SettingsClass ? CreateCalls : std::vector<std::pair<uintptr_t, uintptr_t>>())
            {
                if (Target != Found && FirstCall(Target) == SettingsClass)
                {
                    Found = Target;
                    ++FoundMatches;
                }
            }
            return FoundMatches == 1 ? Found : 0;
        };
        if (Matches > 1)
        {
            // 5.0 (UE 4.21) has a second such pair in StartGameInstance (CL-4214610 0x23C4781, after a call it makes
            // first too): the settings class is the one CreateGameModeForURL's GetGlobalDefaultGameMode starts with, a
            // getter other than GetGameDefaultMap (the false pair's function is called from both).
            Matches = 0;
            for (const auto& [Target, TargetFirst] : Candidates)
            {
                if (const uintptr_t GlobalGetter = FindGlobalDefaultGameMode(TargetFirst); GlobalGetter && GlobalGetter != Target)
                {
                    StaticClass = TargetFirst;
                    GameDefaultMap = Target;
                    ++Matches;
                }
            }
        }
        if (Matches != 1)
        {
            GameDefaultMap = 0;
            StaticClass = 0;
        }
        Require(L"UGameMapsSettings::GetGameDefaultMap", Natives::UGameMapsSettings_GetGameDefaultMap, GameDefaultMap);

        const uintptr_t GlobalDefaultGameMode = FindGlobalDefaultGameMode(StaticClass);
        Require(L"UGameMapsSettings::GetGlobalDefaultGameMode", Natives::UGameMapsSettings_GetGlobalDefaultGameMode, GlobalDefaultGameMode);
    }

    // Whether the code between a null check of [Base+disp] (at Check, Length bytes) and the virtual call at Call loads the
    // vtable of that same object: `mov rX, [Base]`, or `mov rcx, Base` then `mov rX, [rcx]`. That is the inlined
    // UClass::GetDefaultObject's `if (!ClassDefaultObject) CreateDefaultObject();`, where `this` is the class tested.
    bool CallsSameObject(uintptr_t Check, size_t Length, uintptr_t Call)
    {
        const uint8_t Base = static_cast<uint8_t>((Byte(Check + 2) & 0x07) | ((Byte(Check) & 0x01) << 3));
        const auto LoadsVtable = [&](uint8_t Register)
        {
            if ((Register & 0x07) == 0x04 || (Register & 0x07) == 0x05)
            {
                return false;
            }
            for (uintptr_t At = Check + Length; At + 3 <= Call; ++At)
            {
                // REX.W [+R] [+B] 8B /r with mod 00 and r/m the register.
                if ((Byte(At) & 0xFA) == 0x48 && ((Byte(At) & 0x01) << 3) == (Register & 0x08) && Byte(At + 1) == 0x8B &&
                    (Byte(At + 2) & 0xC0) == 0x00 && (Byte(At + 2) & 0x07) == (Register & 0x07))
                {
                    return true;
                }
            }
            return false;
        };
        if (LoadsVtable(Base))
        {
            return true;
        }
        for (uintptr_t At = Check + Length; At + 3 <= Call; ++At)
        {
            // mov rcx, Base: REX.W 8B C8+r (r/m = Base) or REX.W 89 C1+(r<<3) (reg = Base).
            const bool bMovCxFrom8B = (Byte(At) & 0xFE) == 0x48 && ((Byte(At) & 0x01) << 3) == (Base & 0x08) &&
                Byte(At + 1) == 0x8B && Byte(At + 2) == (0xC8 | (Base & 0x07));
            const bool bMovCxFrom89 = (Byte(At) & 0xFB) == 0x48 && ((Byte(At) & 0x04) << 1) == (Base & 0x08) &&
                Byte(At + 1) == 0x89 && Byte(At + 2) == (0xC1 | ((Base & 0x07) << 3));
            if (bMovCxFrom8B || bMovCxFrom89)
            {
                return LoadsVtable(0x01);
            }
        }
        return false;
    }

    // Votes on the vtable slot called right after the inlined UClass::GetDefaultObject's null check of the CDO at DefaultObject.
    // bSameObject counts only the sites whose call goes through the vtable of the object that was checked.
    bool VoteCreateDefaultObject(int32_t DefaultObject, int32_t& OutWinner, int32_t& OutWinnerVotes, int32_t& OutTotal, bool bSameObject = false)
    {
        const uintptr_t Begin = Finder::TextBegin();
        const uintptr_t End = Finder::TextEnd();

        // One scan of the code collects the votes of every candidate offset (+0xC0 to +0x140), which
        // ResolveCreateDefaultObject asks about one after another: a scan each made the packed 5.0 server miss the
        // engine before its creation.
        struct FVotes
        {
            std::unordered_map<int32_t, int32_t> Votes;
            int32_t Total = 0;
        };
        static std::unordered_map<int32_t, FVotes> Cache;
        static std::unordered_map<int32_t, FVotes> SameObjectCache;
        static bool bScanned = false;
        constexpr int32_t MinOffset = 0xC0;
        constexpr int32_t MaxOffset = 0x140;
        if (bScanned && DefaultObject >= MinOffset && DefaultObject <= MaxOffset)
        {
            const FVotes& Cached = bSameObject ? SameObjectCache[DefaultObject] : Cache[DefaultObject];
            int32_t CachedWinner = -1;
            int32_t CachedWinnerVotes = 0;
            for (const auto& [Displacement, Count] : Cached.Votes)
            {
                if (Count > CachedWinnerVotes)
                {
                    CachedWinner = Displacement;
                    CachedWinnerVotes = Count;
                }
            }
            OutWinner = CachedWinner;
            OutWinnerVotes = CachedWinnerVotes;
            OutTotal = Cached.Total;
            return CachedWinner > 0 && CachedWinner % 8 == 0 && CachedWinnerVotes * 10 >= Cached.Total * 9;
        }
        const bool bFill = DefaultObject >= MinOffset && DefaultObject <= MaxOffset;

        std::unordered_map<int32_t, int32_t> Votes;
        int32_t Total = 0;
        for (uintptr_t At = Begin; At + 8 <= End; ++At)
        {
            const int32_t Displacement = *reinterpret_cast<const int32_t*>(At + 3);
            const bool bCandidate = bFill ? Displacement >= MinOffset && Displacement <= MaxOffset && Displacement % 8 == 0 : Displacement == DefaultObject;
            if ((Byte(At) & 0xF8) != 0x48 || (Byte(At + 2) & 0xC0) != 0x80 || (Byte(At + 2) & 0x07) == 0x04 || !bCandidate)
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
                    if (bFill)
                    {
                        FVotes& Entry = Cache[Displacement];
                        ++Entry.Votes[*reinterpret_cast<const int32_t*>(Call + 2)];
                        ++Entry.Total;
                        if (CallsSameObject(At, Length, Call))
                        {
                            FVotes& SameObjectEntry = SameObjectCache[Displacement];
                            ++SameObjectEntry.Votes[*reinterpret_cast<const int32_t*>(Call + 2)];
                            ++SameObjectEntry.Total;
                        }
                    }
                    else if (!bSameObject || CallsSameObject(At, Length, Call))
                    {
                        ++Votes[*reinterpret_cast<const int32_t*>(Call + 2)];
                        ++Total;
                    }
                    break;
                }
            }
        }
        if (bFill)
        {
            bScanned = true;
            return VoteCreateDefaultObject(DefaultObject, OutWinner, OutWinnerVotes, OutTotal, bSameObject);
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

        OutWinner = Winner;
        OutWinnerVotes = WinnerVotes;
        OutTotal = Total;
        return Winner > 0 && Winner % 8 == 0 && WinnerVotes * 10 >= Total * 9;
    }

    void ResolveCreateDefaultObject()
    {
        int32_t Winner = -1;
        int32_t WinnerVotes = 0;
        int32_t Total = 0;
        bool bPassed = VoteCreateDefaultObject(ObjectLayout::Current.UClass_ClassDefaultObject, Winner, WinnerVotes, Total);
        if (!bPassed)
        {
            // 1.10's UClass holds the CDO at +0xF8: the true offset carries hundreds of agreeing sites, every other a handful.
            // Each offset's vote is kept for the 5.0 rule below, so every offset is scanned once.
            struct FOffsetVote
            {
                int32_t Offset = 0;
                bool bPassed = false;
                int32_t Winner = -1;
                int32_t Votes = 0;
                int32_t Total = 0;
            };
            std::vector<FOffsetVote> OffsetVotes;
            const int32_t Default = ObjectLayout::Current.UClass_ClassDefaultObject;
            for (int32_t Offset = Default - 0x20; Offset <= Default + 0x20; Offset += static_cast<int32_t>(sizeof(void*)))
            {
                if (Offset == Default)
                {
                    continue;
                }
                FOffsetVote& Vote = OffsetVotes.emplace_back();
                Vote.Offset = Offset;
                Vote.bPassed = VoteCreateDefaultObject(Offset, Vote.Winner, Vote.Votes, Vote.Total);
                if (Vote.bPassed && Vote.Votes >= 100 && Vote.Votes > WinnerVotes)
                {
                    ObjectLayout::Current.UClass_ClassDefaultObject = Offset;
                    Winner = Vote.Winner;
                    WinnerVotes = Vote.Votes;
                    Total = Vote.Total;
                    bPassed = true;
                }
            }
            if (!bPassed)
            {
                // 5.0 (UE 4.21) inlines GetDefaultObject far less: CL-4214610 has 41 of 42 sites at +0xF8 agreeing on
                // slot 0x65, and no other offset passes. Take the one passing offset with clearly the most sites.
                const FOffsetVote* Best = nullptr;
                int32_t RunnerUpVotes = 0;
                for (const FOffsetVote& Vote : OffsetVotes)
                {
                    if (!Vote.bPassed)
                    {
                        continue;
                    }
                    if (!Best || Vote.Votes > Best->Votes)
                    {
                        RunnerUpVotes = Best ? Best->Votes : 0;
                        Best = &Vote;
                    }
                    else
                    {
                        RunnerUpVotes = (std::max)(RunnerUpVotes, Vote.Votes);
                    }
                }
                if (Best && Best->Votes >= 30 && Best->Votes < RunnerUpVotes * 4)
                {
                    // 5.10 (CL-4240749): +0x110 also passes, 11 of 12 sites on slot 0x47, against +0xF8's 40 of 41 on
                    // 0x65. Those sites call through another object (loaded from [rbx+0xA8]); count only the sites
                    // that call through the object whose member was checked, as GetDefaultObject does.
                    int32_t SameWinner = -1;
                    int32_t SameVotes = 0;
                    int32_t SameTotal = 0;
                    const bool bBestSame = VoteCreateDefaultObject(Best->Offset, SameWinner, SameVotes, SameTotal, true) && SameWinner == Best->Winner;
                    int32_t SameRunnerUp = 0;
                    for (const FOffsetVote& Vote : OffsetVotes)
                    {
                        int32_t OtherWinner = -1;
                        int32_t OtherVotes = 0;
                        int32_t OtherTotal = 0;
                        if (&Vote != Best && Vote.bPassed && VoteCreateDefaultObject(Vote.Offset, OtherWinner, OtherVotes, OtherTotal, true))
                        {
                            SameRunnerUp = (std::max)(SameRunnerUp, OtherVotes);
                        }
                    }
                    RunnerUpVotes = bBestSame && SameVotes >= 30 && SameVotes >= SameRunnerUp * 4 ? 0 : RunnerUpVotes;
                }
                if (Best && Best->Votes >= 30 && Best->Votes >= RunnerUpVotes * 4)
                {
                    ObjectLayout::Current.UClass_ClassDefaultObject = Best->Offset;
                    Winner = Best->Winner;
                    WinnerVotes = Best->Votes;
                    Total = Best->Total;
                    bPassed = true;
                }
            }
            if (bPassed)
            {
                Log::Info(L"Object layout: UClass::ClassDefaultObject at +0x{:X}", ObjectLayout::Current.UClass_ClassDefaultObject);
            }
        }

        if (bPassed)
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
        if (!Matches)
        {
            // 1.9.1's SRand is a leaf with no .pdata entry and nop padding before it.
            for (uintptr_t Site = FindInstruction(Finder::TextBegin(), Finder::TextEnd(), { 0x69, 0x0D, -1, -1, -1, -1, 0x35, 0x84, 0xB3, 0x0B }); Site; Site = FindInstruction(Site + 1, Finder::TextEnd(), { 0x69, 0x0D, -1, -1, -1, -1, 0x35, 0x84, 0xB3, 0x0B }))
            {
                if (!Finder::FunctionStart(Site) && Byte(Site - 1) == 0x90 && FindInstruction(Site + 10, Site + 0x20, { 0x81, 0xC1, 0x6B, 0x63, 0x19, 0x36 }))
                {
                    SRand = Site;
                    ++Matches;
                }
            }
        }
        if (!Matches)
        {
            // 1.10's has no .pdata entry either, and its padding is filler bytes, so it is known by being called.
            for (uintptr_t Site = FindInstruction(Finder::TextBegin(), Finder::TextEnd(), { 0x69, 0x0D, -1, -1, -1, -1, 0x35, 0x84, 0xB3, 0x0B }); Site; Site = FindInstruction(Site + 1, Finder::TextEnd(), { 0x69, 0x0D, -1, -1, -1, -1, 0x35, 0x84, 0xB3, 0x0B }))
            {
                if (!Finder::FunctionStart(Site) && FindInstruction(Site + 10, Site + 0x20, { 0x81, 0xC1, 0x6B, 0x63, 0x19, 0x36 }) && !Finder::CallsTo(Site, false).empty())
                {
                    SRand = Site;
                    ++Matches;
                }
            }
        }
        if (!Matches)
        {
            // 5.0 (UE 4.21) works in eax: imul eax, [GSRandSeed], 0BB38435h; add eax, 3619636Bh; mov [GSRandSeed], eax, a
            // called leaf (CL-4204761 0x14F72A0, CL-4214610 0x14F78A0 with no .pdata entry). Inlined copies store the
            // seed back too, so only a function's first instruction, or a called one with no .pdata entry, counts.
            for (uintptr_t Site = FindInstruction(Finder::TextBegin(), Finder::TextEnd(), { 0x69, 0x05, -1, -1, -1, -1, 0x35, 0x84, 0xB3, 0x0B, 0x05, 0x6B, 0x63, 0x19, 0x36, 0x89, 0x05 }); Site; Site = FindInstruction(Site + 1, Finder::TextEnd(), { 0x69, 0x05, -1, -1, -1, -1, 0x35, 0x84, 0xB3, 0x0B, 0x05, 0x6B, 0x63, 0x19, 0x36, 0x89, 0x05 }))
            {
                const uintptr_t Start = Finder::FunctionStart(Site);
                if (Finder::RelativeTarget(Site, 2, 10) == Finder::RelativeTarget(Site + 15, 2, 6) && (Start == Site || (!Start && !Finder::CallsTo(Site, false).empty())))
                {
                    SRand = Site;
                    ++Matches;
                }
            }
        }
        Require(L"FMath::SRand", Natives::FMath_SRand, Matches == 1 ? SRand : 0);

        // 5.0 (UE 4.21) inlines AllocateSerialNumber into FWeakObjectPtr::operator=(const UObject*), which then holds the
        // log (CL-4214610 0x178A990): test rdx, rdx; jz; mov ecx, [rdx+0Ch] (InternalIndex); mov [rsi], ecx, with rsi the
        // pointer. Core's FWeakObjectPtr calls that instead.
        const uintptr_t SerialNumbers = FunctionOf(L"UObject serial numbers overflowed (trying to allocate serial number %d).");
        const uintptr_t NullTest = SerialNumbers ? FindInstruction(SerialNumbers, SerialNumbers + 0x20, { 0x48, 0x85, 0xD2 }) : 0;
        const uintptr_t IndexCopy = NullTest ? FindInstruction(NullTest, NullTest + 0x14, { 0x8B, 0x4A, 0x0C, 0x89 }) : 0;
        bool bAssign = IndexCopy && (Byte(IndexCopy + 4) & 0xF8) == 0x08;
        // 6.10 (CL-4480234 0x18F2600) keeps the index in eax and stores it through rcx: mov eax, [rdx+0Ch]; mov [rcx],
        // eax. Whichever register carries it, the object's index loaded and stored straight through a pointer is the
        // assignment; AllocateSerialNumber itself takes the index in edx and never reads through rdx.
        for (uintptr_t Site = bAssign ? 0 : NullTest; Site && Site < NullTest + 0x14 && !bAssign; ++Site)
        {
            const uint8_t Load = Byte(Site + 1);
            const uint8_t Store = Byte(Site + 4);
            bAssign = Byte(Site) == 0x8B && (Load & 0xC7) == 0x42 && Byte(Site + 2) == 0x0C && Byte(Site + 3) == 0x89
                && (Store & 0xC0) == 0x00 && (Store & 0x38) == (Load & 0x38) && (Store & 0x07) != 0x04 && (Store & 0x07) != 0x05;
        }
        Natives::FWeakObjectPtr_Assign = nullptr;
        if (bAssign)
        {
            Natives::FWeakObjectPtr_Assign = reinterpret_cast<decltype(Natives::FWeakObjectPtr_Assign)>(SerialNumbers);
            Log::Info(L"Finder: FUObjectArray::AllocateSerialNumber is inlined into FWeakObjectPtr::operator= (+0x{:X}), which Core calls", SerialNumbers - Finder::ImageBase());
        }
        else
        {
            Require(L"FUObjectArray::AllocateSerialNumber", Natives::FUObjectArray_AllocateSerialNumber, SerialNumbers);
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

        // mov r13b, 1 (r12b on 1.10); mov [GIsServer], sil; mov [GIsClient], r13b.
        for (uintptr_t Site = FindInstruction(PreInit, PreInitEnd, { 0x41, -1, 0x01, 0x40, 0x88, 0x35 }); Site; Site = FindInstruction(Site + 1, PreInitEnd, { 0x41, -1, 0x01, 0x40, 0x88, 0x35 }))
        {
            const uintptr_t ClientStore = Site + 10;
            const uint8_t Register = Byte(Site + 1) & 0x07;
            if ((Byte(Site + 1) & 0xF8) == 0xB0 && Byte(ClientStore) == 0x44 && Byte(ClientStore + 1) == 0x88 && Byte(ClientStore + 2) == (0x05 | Register << 3) && Finder::RelativeTarget(ClientStore, 3, 7) == GIsClient)
            {
                Sites::PreInitRegularClient = reinterpret_cast<uint8_t*>(Site);
                break;
            }
        }
        if (!Sites::PreInitRegularClient && Packed::IsPacked())
        {
            // A packed build's PreInit has run, untouched, before Core starts (FEngineLoop::HookAfterPreInit sets the
            // globals instead), so the block is never patched there. 4.2 keeps the 1 in r13b through PreInit and stores
            // GIsClient before GIsServer (0x2AD522), a shape the patch doesn't take.
            Log::Info(L"Finder: PreInit regular-client block not found, unused on a packed build");
        }
        else if (!Sites::PreInitRegularClient)
        {
            Log::Error(L"Finder: PreInit regular-client block not found");
            ++MissingCount;
        }
    }

    // bWide: 5.0 (UE 4.21) schedules more between the compare and its jnz (UGameEngine::Tick, CL-4214610 0x23C60AF: jnz
    // 0x12 on); the wider scan stops at a jz, the other branch of a compare that isn't such a check.
    // bDecode: instruction by instruction, not byte by byte. On 5.21 (CL-4288479) the displacements of the flag compares
    // after two of UGameEngine::Tick's commandlet compares hold a 0x75 (0x248B4EC, 0x248B547), which the byte scan read as
    // jnz, three checks where there are two.
    std::vector<uint8_t*> CommandletChecks(uintptr_t Function, bool bWide = false, bool bDecode = false)
    {
        std::vector<uint8_t*> Checks;
        const uintptr_t End = Finder::FunctionEnd(Function);
        const uintptr_t Commandlet = reinterpret_cast<uintptr_t>(Natives::PRIVATE_GIsRunningCommandlet);
        const auto IsJnz = [](uintptr_t At) { return Byte(At) == 0x75 || (Byte(At) == 0x0F && Byte(At + 1) == 0x85); };
        const auto IsJz = [](uintptr_t At) { return Byte(At) == 0x74 || (Byte(At) == 0x0F && Byte(At + 1) == 0x84); };
        for (uintptr_t Compare = Function; Function && End && Compare + 6 <= End; ++Compare)
        {
            if (Byte(Compare) != 0x38 || (Byte(Compare + 1) & 0xC7) != 0x05 || Finder::RelativeTarget(Compare, 2, 6) != Commandlet)
            {
                continue;
            }
            const uintptr_t Limit = Compare + (bWide ? 0x18 : 0x12);
            if (bDecode)
            {
                const uintptr_t Jump = Finder::TraceFind(Compare + 6, 8, [&](uintptr_t At)
                {
                    return At < Compare + 6 || At >= Limit || IsJnz(At) || (bWide && IsJz(At));
                });
                if (Jump >= Compare + 6 && Jump < Limit && IsJnz(Jump))
                {
                    Checks.push_back(reinterpret_cast<uint8_t*>(Jump));
                }
                continue;
            }
            for (uintptr_t Jump = Compare + 6; Jump < Limit; ++Jump)
            {
                if (bWide && IsJz(Jump))
                {
                    break;
                }
                if (IsJnz(Jump))
                {
                    Checks.push_back(reinterpret_cast<uint8_t*>(Jump));
                    break;
                }
            }
        }
        return Checks;
    }

    void RequireChecks(const wchar_t* Name, std::vector<uint8_t*>& Destination, std::vector<uint8_t*> Checks, size_t Expected, size_t MaxExpected = 0)
    {
        if (Checks.size() < Expected || Checks.size() > std::max(Expected, MaxExpected))
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
            const uint8_t Register = Sites::PreInitRegularClient[1] & 0x07;
            const int32_t Test = 0xC0 | Register << 3 | Register;
            for (uintptr_t At = FindInstruction(reinterpret_cast<uintptr_t>(Sites::PreInitRegularClient), PreInitEnd, { 0x45, 0x84, Test, 0x0F, 0x84 }); At; At = FindInstruction(At + 1, PreInitEnd, { 0x45, 0x84, Test, 0x0F, 0x84 }))
            {
                PreInitChecks.push_back(reinterpret_cast<uint8_t*>(At + 3));
            }
        }
        // 1.10's PreInit guards a second movie player call (0x291B32) as well. Like the regular-client block, these are
        // only patched on a build that isn't packed; a packed one without the block has no use for them.
        if (Sites::PreInitRegularClient || !Packed::IsPacked())
        {
            RequireChecks(L"FEngineLoop::PreInit", Sites::PreInitDedicatedServerChecks, std::move(PreInitChecks), 5, 6);
        }
        RequireChecks(L"UEngine::Init", Sites::EngineInitDedicatedServerChecks, CommandletChecks(FunctionOf(L"Initializing Engine...")), 2);
        std::vector<uint8_t*> TickChecks = CommandletChecks(reinterpret_cast<uintptr_t>(Natives::UGameEngine_Tick));
        if (TickChecks.size() < 2)
        {
            TickChecks = CommandletChecks(reinterpret_cast<uintptr_t>(Natives::UGameEngine_Tick), true);
        }
        if (TickChecks.size() > 2)
        {
            TickChecks = CommandletChecks(reinterpret_cast<uintptr_t>(Natives::UGameEngine_Tick), false, true);
            if (TickChecks.size() < 2)
            {
                TickChecks = CommandletChecks(reinterpret_cast<uintptr_t>(Natives::UGameEngine_Tick), true, true);
            }
        }
        RequireChecks(L"UGameEngine::Tick", Sites::GameEngineTickDedicatedServerChecks, std::move(TickChecks), 2);
    }

    void ResolveOutputDevices()
    {
        uintptr_t LogSingleton = 0;
        uintptr_t AddOutputDevice = 0;
        const uintptr_t Ref = OnlyRef(L"AllowStdOutLogVerbosity");
        uintptr_t Tail = Ref ? FindInstruction(Ref, Ref + 0x100, { 0xE8, -1, -1, -1, -1, 0x48, 0x8B, 0xC8, 0x48, 0x8B, 0xD3, 0x48, 0x83, 0xC4, -1, 0x5B, 0xE9 }) : 0;
        if (Ref && !Tail)
        {
            // 1.10 calls the FOutputDeviceStdOutput constructor (0x288BC0) instead of inlining it; the tail follows that call.
            const std::vector<uintptr_t> Calls = Finder::CallsTo(Finder::FunctionStart(Ref), false);
            if (Calls.size() == 1)
            {
                Tail = FindInstruction(Calls.front(), Calls.front() + 0x100, { 0xE8, -1, -1, -1, -1, 0x48, 0x8B, 0xC8, 0x48, 0x8B, 0xD3, 0x48, 0x83, 0xC4, -1, 0x5B, 0xE9 });
            }
        }
        if (Tail)
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
            // UE_LOG puts the format on the stack and calls Logf_Internal next, but the packer splits the block: on
            // 2.2 to 2.4 the "LoadMap: %s" half sits in the moved code, where a scan for the nearest call found
            // nothing and the other anchor carried it, and 2.4.2 leaves it in .text, where that scan ran past the
            // jmp to the call and answered with the unrelated function behind it. Follow the jmp instead.
            const uintptr_t Call = Finder::TraceFind(Ref, 16, [](uintptr_t Instruction) { return Byte(Instruction) == 0xE8; });
            // 1.1 to 1.9 have no moved code for TraceFind to follow a jmp into, so the nearest call still answers there.
            const uintptr_t Target = Call ? Finder::RelativeTarget(Call, 1, 5) : Finder::NextCall(Ref, 0x40);
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
        // 1.10's formats the expression with "%s" and only keeps the full message.
        uintptr_t AssertFailed = FunctionOf(L"Assertion failed: %s");
        if (!AssertFailed)
        {
            AssertFailed = FunctionOf(L"Assertion failed: %s [File:%s] [Line: %i] \n%s\n");
        }
        Require(L"FDebug::AssertFailed", Natives::FDebug_AssertFailed, AssertFailed);
        Require(L"LowLevelFatalErrorHandler", Natives::LowLevelFatalErrorHandler, FunctionOf(L"LowLevelFatalError"));
    }
}

bool Finder::ResolveRuntime()
{
    MissingCount = 0;
    Memcury::Scanner::SetTargetModule(nullptr);

    ResolveGUObjectArray();
    if (!Natives::GUObjectArray)
    {
        // The other runtime finders start from code this one reads too: past it they'd scan from address 0 and crash.
        Log::Error(L"Finder: runtime resolution stopped at GUObjectArray; the exe's code isn't one Core can read");
        return false;
    }
    ResolveNames();
    ResolveObjectGlobals();

    // UE 4.18's FSoftObjectPath (1.10) replaces FStringAssetReference, whose name 1.1-1.9.1 hold instead.
    if (Finder::FindReadOnly(L"SoftObjectPath", sizeof(L"SoftObjectPath")))
    {
        ObjectLayout::Current.bSoftObjectPaths = true;
        Log::Info(L"Object layout: asset paths are FSoftObjectPath");
    }
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
        // 4.2 inlines AddStatic into the registration (0x6E944E): it makes the delegate instance there (lea rax, Handler;
        // mov [rdi+8], rax ... lea rax, its vtable; mov [rdi], rax), then adds it to the multicast delegate, whose
        // compaction threshold it lowers first: mov eax, [Threshold]; mov edx, [ArrayNum]; dec eax; mov [Threshold], eax.
        // 4.2 also takes UE 4.21's NUM_DELEGATE_INLINE_BYTES 0: the invocation list is a heap TArray (Data 0x4917F50,
        // ArrayNum 0x4917F58, CompactionThreshold 0x4917F60) of 0x10-byte FDelegateBases, where up to 4.1 ArrayNum is
        // 0x10 before the threshold at +0x60 (#303: Core read 4.2's delegate as 4.1's and the console crashed when opened).
        uintptr_t InlineBind = 0;
        if (!Entries && Handler && Matches == 0)
        {
            int32_t LeaMatches = 0;
            const uintptr_t HandlerLea = ScanRipTarget({ 0x48, 0x8D, 0x05 }, 3, 7, Handler, LeaMatches);
            for (uintptr_t Load = LeaMatches == 1 ? FindInstruction(HandlerLea + 7, HandlerLea + 0x40, { 0x8B, 0x05 }) : 0; Load && !Entries; Load = FindInstruction(Load + 1, HandlerLea + 0x40, { 0x8B, 0x05 }))
            {
                const uintptr_t Threshold = Finder::RelativeTarget(Load, 2, 6);
                const uintptr_t Store = FindInstruction(Load + 6, Load + 0x14, { 0x89, 0x05 });
                if (!Store || Finder::RelativeTarget(Store, 2, 6) != Threshold || IsInText(Threshold) || *reinterpret_cast<const int32_t*>(Threshold) != 2)
                {
                    continue;
                }
                uintptr_t ArrayNum = 0;
                for (uintptr_t Num = Load + 6; Num + 6 <= Store && !ArrayNum; ++Num)
                {
                    if (Byte(Num) == 0x8B && (Byte(Num + 1) & 0xC7) == 0x05)
                    {
                        ArrayNum = Finder::RelativeTarget(Num, 2, 6);
                    }
                }
                if (ArrayNum == Threshold - 0x08)
                {
                    Natives::NumDelegateInlineBytes = 0;
                    Entries = Threshold - 0x10;
                    Log::Info(L"Finder: delegates use the heap allocator (NUM_DELEGATE_INLINE_BYTES 0)");
                }
                else if (ArrayNum == Threshold - 0x10)
                {
                    Entries = Threshold - 0x60;
                }
                InlineBind = Entries ? HandlerLea : 0;
            }
            // 5.0 (UE 4.21) makes the instance inline as 4.2 does but adds it through a call: lea rax, Handler; ...; lea
            // rax, its vtable; mov [rbx], rax; lea rcx, [Entries]; call AddInternal (CL-4214610 0x79BF9D, 0x79BFC3),
            // whose compaction threshold (2) is at +0x10 after Data and ArrayNum: dec dword ptr [rcx+10h].
            for (uintptr_t Lea = !Entries && LeaMatches == 1 ? FindInstruction(HandlerLea + 7, HandlerLea + 0x40, { 0x48, 0x8D, 0x0D }) : 0; Lea && !Entries; Lea = FindInstruction(Lea + 1, HandlerLea + 0x40, { 0x48, 0x8D, 0x0D }))
            {
                const uintptr_t Delegate = Finder::RelativeTarget(Lea, 3, 7);
                const uintptr_t AddCall = FindRawCall(Lea + 7, Lea + 0x10);
                const uintptr_t Add = AddCall ? Finder::RelativeTarget(AddCall, 1, 5) : 0;
                if (!IsInText(Delegate) && Add && FindInstruction(Add, Add + 0x30, { 0xFF, 0x49, 0x10 }) && *reinterpret_cast<const int32_t*>(Delegate + 0x10) == 2)
                {
                    Natives::NumDelegateInlineBytes = 0;
                    Entries = Delegate;
                    InlineBind = HandlerLea;
                    Log::Info(L"Finder: delegates use the heap allocator (NUM_DELEGATE_INLINE_BYTES 0)");
                }
            }
        }
        RequireAddress(L"UConsole::RegisterConsoleAutoCompleteEntries", reinterpret_cast<uintptr_t&>(Natives::UConsole_RegisterConsoleAutoCompleteEntries), Entries);

        // The delegate instance layout, from the static instance AddStatic binds the handler with (2.5 0x6A8610, 2.4.2
        // 0x2EBB00): GetHandle (mov rax, [rcx+Handle]; mov [rdx], rax; mov rax, rdx; ret) is slot 8 up to 2.4.2 and 4 on
        // 2.5 (0x30C79F8), where UE 4.20 dropped five virtuals before it (#192). Not found keeps 8.
        // 4.2's inline instance: its vtable is the lea after the handler's (0x6E9462).
        const uintptr_t AddStaticCall = Entries && !InlineBind ? FindRawCall(Bind + 7, Bind + 0x20) : 0;
        const uintptr_t AddStatic = AddStaticCall ? Finder::RelativeTarget(AddStaticCall, 1, 5) : InlineBind ? InlineBind + 7 : 0;
        const uintptr_t AddStaticScanEnd = InlineBind ? InlineBind + 0x20 : AddStatic + 0x100;
        int32_t GetHandleIndex = -1;
        for (uintptr_t Lea = AddStatic ? FindInstruction(AddStatic, AddStaticScanEnd, { 0x48, 0x8D, 0x05 }) : 0; Lea && GetHandleIndex < 0; Lea = FindInstruction(Lea + 1, AddStaticScanEnd, { 0x48, 0x8D, 0x05 }))
        {
            void* const* const VTable = reinterpret_cast<void* const*>(Finder::RelativeTarget(Lea, 3, 7));
            if (IsInText(reinterpret_cast<uintptr_t>(VTable)))
            {
                continue;
            }
            for (int32_t Index = 0; Index < 0x10 && GetHandleIndex < 0; ++Index)
            {
                const uintptr_t Function = reinterpret_cast<uintptr_t>(VTable[Index]);
                if (IsInText(Function) && FindInstruction(Function, Function + 11, { 0x48, 0x8B, 0x41, -1, 0x48, 0x89, 0x02, 0x48, 0x8B, 0xC2, 0xC3 }) == Function)
                {
                    GetHandleIndex = Index;
                }
            }
        }
        // 5.30 (CL-4305896): slot 5, after GetUObject and the new GetObjectForTimerManager (DelegateInstance::
        // SlotsWithTimerManagerObject).
        if (GetHandleIndex == 4 || GetHandleIndex == 5 || GetHandleIndex == 8)
        {
            Natives::IDelegateInstance_GetHandleIndex = GetHandleIndex;
            Natives::IDelegateInstance_GetHandleIndexResolved = true;
            Log::Info(L"Finder: IDelegateInstance::GetHandle is slot {}", GetHandleIndex);
        }
        else
        {
            Log::Error(L"Finder: IDelegateInstance::GetHandle not found (slot {}), using slot 8", GetHandleIndex);
        }


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
        if (UserName)
        {
            Require(L"FPlatformProcess::UserName", Natives::FPlatformProcess_UserName, UserName);
        }
        else
        {
            // The engine function could not be located (its calls to GetUserNameW may be
            // inlined or routed differently in this build).  Fall back to a local
            // implementation that calls GetUserNameW directly through its import slot.
            Natives::FPlatformProcess_UserName = [](bool bOnlyAlphaNumeric) -> const wchar_t*
            {
                static wchar_t Full[256];
                static wchar_t AlphaNumeric[256];
                static bool Ready = false;
                if (!Ready)
                {
                    DWORD Length = static_cast<DWORD>(std::size(Full));
                    if (!::GetUserNameW(Full, &Length))
                    {
                        Full[0] = L'\0';
                    }
                    wchar_t* Write = AlphaNumeric;
                    for (const wchar_t* Read = Full; *Read; ++Read)
                    {
                        if ((*Read >= L'0' && *Read <= L'9') ||
                            (*Read >= L'A' && *Read <= L'Z') ||
                            (*Read >= L'a' && *Read <= L'z'))
                        {
                            *Write++ = *Read;
                        }
                    }
                    *Write = L'\0';
                    Ready = true;
                }
                return bOnlyAlphaNumeric ? AlphaNumeric : Full;
            };
            Log::Info(L"Finder: FPlatformProcess::UserName fell back to GetUserNameW");
        }

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
        // mov byte ptr [rcx+X], 0; mov rcx, [GEngine]; lea rax, ... The flag is at +0x78 up to 2.4.2 (disp8), +0x80 on 2.5
        // (0x200D690, disp32).
        for (uintptr_t Load = GEngine ? FindInstruction(Finder::TextBegin(), Finder::TextEnd(), { 0x48, 0x8B, 0x0D }) : 0; Load; Load = FindInstruction(Load + 1, Finder::TextEnd(), { 0x48, 0x8B, 0x0D }))
        {
            const bool bStore = (Byte(Load - 4) == 0xC6 && Byte(Load - 3) == 0x41 && Byte(Load - 1) == 0x00)
                || (Byte(Load - 7) == 0xC6 && Byte(Load - 6) == 0x81 && Byte(Load - 1) == 0x00 && Byte(Load - 2) == 0x00 && Byte(Load - 3) == 0x00);
            if (bStore && Finder::RelativeTarget(Load, 3, 7) == GEngine && Byte(Load + 7) == 0x48 && Byte(Load + 8) == 0x8D && Byte(Load + 9) == 0x05)
            {
                Setup = Finder::FunctionStart(Load);
                ++Matches;
            }
        }
        // 4.2 (0x210F300) schedules the clear and the GEngine load apart: mov byte ptr [rcx+80h], 0 (0x210F31A), then
        // mov rdi, [GEngine] (0x210F333) to add the viewport's delegates to the engine's.
        for (uintptr_t Store = GEngine && Matches == 0 ? FindInstruction(Finder::TextBegin(), Finder::TextEnd(), { 0xC6, 0x81, 0x80, 0x00, 0x00, 0x00, 0x00 }) : 0; Store;
             Store = FindInstruction(Store + 1, Finder::TextEnd(), { 0xC6, 0x81, 0x80, 0x00, 0x00, 0x00, 0x00 }))
        {
            for (uintptr_t Load = Store + 7; Load < Store + 7 + 0x30; ++Load)
            {
                if (Byte(Load) == 0x48 && Byte(Load + 1) == 0x8B && (Byte(Load + 2) & 0xC7) == 0x05 && Finder::RelativeTarget(Load, 3, 7) == GEngine)
                {
                    Setup = Finder::FunctionStart(Store);
                    ++Matches;
                    break;
                }
            }
        }
        // 5.0 (UE 4.21, CL-4214610 0x23FC7C0) clears the flag at +0x70 (disp8) and loads GEngine into rdi further on to add
        // the viewport's delegate to the engine's travel failure event: mov byte ptr [rcx+70h], 0; ...; mov rdi, [GEngine];
        // lea edx, [r8+30h]; add rdi, 918h.
        for (uintptr_t Store = GEngine && Matches == 0 ? FindInstruction(Finder::TextBegin(), Finder::TextEnd(), { 0xC6, 0x41, -1, 0x00 }) : 0; Store; Store = FindInstruction(Store + 1, Finder::TextEnd(), { 0xC6, 0x41, -1, 0x00 }))
        {
            for (uintptr_t Load = Store + 4; Load < Store + 0x20; ++Load)
            {
                if ((Byte(Load) & 0xFB) == 0x48 && Byte(Load + 1) == 0x8B && (Byte(Load + 2) & 0xC7) == 0x05 && Finder::RelativeTarget(Load, 3, 7) == GEngine
                    && FindInstruction(Load + 7, Load + 0x14, { 0x48, 0x81, 0xC0 | ((Byte(Load + 2) >> 3) & 0x07) }))
                {
                    const uintptr_t Function = Finder::FunctionStart(Store);
                    if (Function && Function != Setup)
                    {
                        Setup = Function;
                        ++Matches;
                    }
                    break;
                }
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
            // A vtable starts with code. 1.8.1's ReplicateActor also calls slot 0x74, which would put a "vtable" on the
            // L"UNetDriver::InitListen" string (0x487FD10) 0x158 bytes before UPackageMapClient's (0x487FE68).
            const uintptr_t VTable = Index >= 0 ? Entry - Index * sizeof(void*) : 0;
            if (VTable && Finder::IsReadable(reinterpret_cast<const void*>(VTable), sizeof(uintptr_t)) && IsInText(*reinterpret_cast<const uintptr_t*>(VTable)))
            {
                Candidates.emplace_back(Index, VTable);
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
        if (!Thunks.empty())
        {
            return Thunks;
        }

        // 1.10 (UE 4.19) registers natives from a static { name, thunk } table in .rdata instead (AActor's at 0x3904000),
        // which StaticRegisterNatives passes to FNativeFunctionRegistrar::RegisterFunctions with a lea (0x1D53D1F). The
        // registrar is the function holding that lea; the FuncInfo tables of { name, constructor } pairs have no code
        // reference to their start and so drop out.
        static std::vector<std::pair<uintptr_t, uintptr_t>> ReadOnlyLeas;
        Memcury::PE::Section ReadOnly = Memcury::PE::Section::GetSection(".rdata");
        const uintptr_t ReadOnlyBegin = ReadOnly.GetSectionStart().Get();
        const uintptr_t ReadOnlyEnd = ReadOnly.GetSectionEnd().Get();
        const auto IsName = [&](uintptr_t At) { const uintptr_t Text = *reinterpret_cast<const uintptr_t*>(At); return Text >= ReadOnlyBegin && Text < ReadOnlyEnd; };
        const auto IsThunk = [](uintptr_t At) { return IsInText(*reinterpret_cast<const uintptr_t*>(At)); };
        const size_t Length = std::strlen(Name) + 1;
        for (uintptr_t At = (ReadOnlyBegin + 7) & ~static_cast<uintptr_t>(7); At + 2 * sizeof(uintptr_t) <= ReadOnlyEnd; At += sizeof(uintptr_t))
        {
            const uintptr_t Text = *reinterpret_cast<const uintptr_t*>(At);
            if (Text < ReadOnlyBegin || Text >= ReadOnlyEnd || Length > ReadOnlyEnd - Text || std::memcmp(reinterpret_cast<const void*>(Text), Name, Length) != 0 || !IsThunk(At + sizeof(uintptr_t)))
            {
                continue;
            }
            uintptr_t Table = At;
            while (Table - 2 * sizeof(uintptr_t) >= ReadOnlyBegin && IsName(Table - 2 * sizeof(uintptr_t)) && IsThunk(Table - sizeof(uintptr_t)))
            {
                Table -= 2 * sizeof(uintptr_t);
            }

            if (ReadOnlyLeas.empty())
            {
                for (uintptr_t Lea = Finder::TextBegin(); Lea + 7 <= Finder::TextEnd(); ++Lea)
                {
                    if ((Byte(Lea) & 0xFB) == 0x48 && Byte(Lea + 1) == 0x8D && (Byte(Lea + 2) & 0xC7) == 0x05)
                    {
                        const uintptr_t Target = Finder::RelativeTarget(Lea, 3, 7);
                        if (Target >= ReadOnlyBegin && Target < ReadOnlyEnd)
                        {
                            ReadOnlyLeas.emplace_back(Target, Lea);
                        }
                    }
                }
                std::sort(ReadOnlyLeas.begin(), ReadOnlyLeas.end());
            }
            const auto Lea = std::lower_bound(ReadOnlyLeas.begin(), ReadOnlyLeas.end(), std::make_pair(Table, uintptr_t(0)));
            if (Lea != ReadOnlyLeas.end() && Lea->first == Table)
            {
                Thunks.emplace_back(Finder::FunctionStart(Lea->second), *reinterpret_cast<const uintptr_t*>(At + sizeof(uintptr_t)));
            }
        }
        return Thunks;
    }

    // The server pass runs before any class default object exists, so it has no Actor vtable to read a slot from, and
    // on a packed exe the virtual call to AActor::IsNetRelevantFor is in moved code that can be ciphertext when Core
    // resolves. Derive the slot from the first .text instead: IsNetRelevantFor sits at the same vtable slot in AActor
    // and in every subclass that doesn't override it, so the slot those vtables agree on most is it. A vtable's base is
    // the first .text address a lea in code loads; the function's own slot is its offset from that base.
    int32_t NetRelevantSlotFromVTables(uintptr_t Function)
    {
        if (!Function)
        {
            return -1;
        }
        const uintptr_t TextBegin = Finder::TextBegin();
        const uintptr_t TextEnd = Finder::TextEnd();
        Memcury::PE::Section ReadOnly = Memcury::PE::Section::GetSection(".rdata");
        const uintptr_t ReadOnlyBegin = ReadOnly.GetSectionStart().Get();
        const uintptr_t ReadOnlyEnd = ReadOnly.GetSectionEnd().Get();

        std::vector<uintptr_t> Bases;
        for (uintptr_t Lea = TextBegin; Lea + 7 <= TextEnd; ++Lea)
        {
            if ((Byte(Lea) & 0xFB) != 0x48 || Byte(Lea + 1) != 0x8D || (Byte(Lea + 2) & 0xC7) != 0x05)
            {
                continue;
            }
            const uintptr_t Target = Finder::RelativeTarget(Lea, 3, 7);
            if (Target >= ReadOnlyBegin && Target < ReadOnlyEnd && (Target & 7) == 0)
            {
                Bases.push_back(Target);
            }
        }
        std::sort(Bases.begin(), Bases.end());
        Bases.erase(std::unique(Bases.begin(), Bases.end()), Bases.end());

        std::unordered_map<int32_t, int32_t> Votes;
        for (uintptr_t At = (ReadOnlyBegin + 7) & ~static_cast<uintptr_t>(7); At + sizeof(uintptr_t) <= ReadOnlyEnd; At += sizeof(uintptr_t))
        {
            if (*reinterpret_cast<const uintptr_t*>(At) != Function)
            {
                continue;
            }
            uintptr_t Base = 0;
            for (uintptr_t Walk = At; Walk >= ReadOnlyBegin && At - Walk < 0x1600 * sizeof(uintptr_t); Walk -= sizeof(uintptr_t))
            {
                if (std::binary_search(Bases.begin(), Bases.end(), Walk))
                {
                    Base = Walk;
                    break;
                }
                if (Walk == ReadOnlyBegin)
                {
                    break;
                }
                const uintptr_t Entry = *reinterpret_cast<const uintptr_t*>(Walk - sizeof(uintptr_t));
                if (Entry < TextBegin || Entry >= TextEnd)
                {
                    break;
                }
            }
            if (Base)
            {
                ++Votes[static_cast<int32_t>((At - Base) / sizeof(uintptr_t))];
            }
        }

        int32_t Best = -1;
        int32_t BestVotes = 0;
        int32_t Ties = 0;
        for (const auto& [Slot, Count] : Votes)
        {
            if (Count > BestVotes)
            {
                BestVotes = Count;
                Best = Slot;
                Ties = 1;
            }
            else if (Count == BestVotes)
            {
                ++Ties;
            }
        }
        return BestVotes > 0 && Ties == 1 ? Best : -1;
    }

    void ResolveReplication()
    {
        const uintptr_t ReplicateActor = FunctionOf(L"ReplicateActor: bPausedUntilReliableACK is ending now that reliables have been ACK'd. %s");
        const uintptr_t ReplicateActorEnd = Finder::FunctionEnd(ReplicateActor);
        const uintptr_t SetChannelActor = FunctionOf(L"SetChannelActor: ChIndex: %i, Actor: %s, NetGUID: %s");
        uintptr_t NetViewer = Finder::FunctionStart(First(Finder::StringRefs("ServerForwardView")));
        // 1.9 (CL-3757339) drops FNetViewer's ahead prediction, the code that named ServerForwardView and read the
        // connection's TickCount and the view target's velocity: its constructor (0x22BC640) only fills in the viewer,
        // and DeltaSeconds is unused. Its one caller, UIpNetDriver::ProcessRemoteFunction (0x3A52490; 1.8.2 0x3A22AA0
        // calls the old constructor 0x22A2E70 the same way), builds a viewer with its first call.
        const bool bNetViewerPredicts = NetViewer != 0;
        if (!NetViewer)
        {
            const uintptr_t RemoteFunction = FunctionOf(L"UIpNetDriver::ProcesRemoteFunction: No owning connection for actor %s. Function %s will not be processed.");
            const uintptr_t Constructor = RemoteFunction ? Finder::NextCall(RemoteFunction, 0x100) : 0;
            NetViewer = Constructor && FindInstruction(Constructor, Constructor + 0x40, { 0x48, 0x89, 0x11 }) ? Constructor : 0;
        }
        if (!NetViewer)
        {
            // On 1.9.1 ProcessRemoteFunction is moved code. The constructor (0x22CFA40) is known by how it
            // fills in the viewer: Connection, then InViewer (the connection's PlayerController, else its OwningActor),
            // then ViewTarget.
            const uintptr_t Fill = UniqueFunction({ 0x48, 0x89, 0x11, 0x48, 0x8B, 0xD9, 0x48, 0x8B, 0x42, -1, 0x48, 0x85, 0xC0, 0x75, -1, 0x48, 0x8B, 0x82, -1, -1, 0x00, 0x00, 0x48, 0x89, 0x41, 0x08, 0x48, 0x8B, 0x82, -1, -1, 0x00, 0x00, 0x48, 0x89, 0x41, 0x10 });
            const uintptr_t Constructor = Finder::FunctionStart(Fill);
            NetViewer = Constructor && Fill - Constructor < 0x20 ? Constructor : 0;
        }
        const uintptr_t NetViewerEnd = Finder::FunctionEnd(NetViewer);
        Require(L"UActorChannel::ReplicateActor", Natives::UActorChannel_ReplicateActor, ReplicateActor);
        Require(L"UActorChannel::SetChannelActor", Natives::UActorChannel_SetChannelActor, SetChannelActor);
        Require(L"FNetViewer::FNetViewer", Natives::FNetViewer_FNetViewer, NetViewer);
        Require(L"FNetGUIDCache::SupportsObject", Natives::FNetGUIDCache_SupportsObject, FunctionOf(L"FNetGUIDCache::SupportsObject: %s NOT Supported."));
        Require(L"FObjectReplicator::ValidateAgainstState", Natives::FObjectReplicator_ValidateAgainstState, FunctionOf(L"ValidateAgainstState: Properties changed for %s"));

        // 1.10's log adds ChSequence, and its call (0x1ACF7A5 to 0x1ABDCD0) sits 0x61 past the string. That string is
        // tried first so builds with it don't warn about the older one being absent.
        uintptr_t CreateChannel = 0;
        const uintptr_t SequencedCreate = First(Finder::StringRefs(L"      Bunch Create %i: ChType %i, ChSequence: %i, bReliable: %i, bPartial: %i, bPartialInitial: %i, bPartialFinal: %i"));
        // 6.10 (CL-4480234, UE 4.22's networking) names its channels: the log prints ChName, and the call under it
        // (0x270201E, 0x46 past the string) is CreateChannelByName(const FName& ChName, EChannelCreateFlags CreateFlags,
        // int32 ChannelIndex) (0x26EDF50). There is no CreateChannel by type left. Looked up only where the ChType
        // string is absent, and before the oldest string so 6.10 doesn't warn about that one.
        const uintptr_t NamedCreate = SequencedCreate ? 0 : First(Finder::StringRefs(L"      Bunch Create %i: ChName %s, ChSequence: %i, bReliable: %i, bPartial: %i, bPartialInitial: %i, bPartialFinal: %i"));
        const uintptr_t BunchCreate = SequencedCreate ? SequencedCreate : NamedCreate ? NamedCreate : OnlyRef(L"      Bunch Create %i: ChType %i, bReliable: %i, bPartial: %i, bPartialInitial: %i, bPartialFinal: %i");
        const uintptr_t BunchCreateEnd = BunchCreate + (SequencedCreate || NamedCreate ? 0x80 : 0x40);
        // 2.4.2 sizes Channels by MaxChannelSize (a TArray, as in 4.20) and logs when no index is free (0x1B0C510).
        const uintptr_t NoFreeChannel = FunctionOf(L"No free channel could be found in the channel list (current limit is %d channels). Consider increasing the max channels allowed using CVarMaxChannelSize.");
        for (uintptr_t Site = BunchCreate ? FindRawCall(BunchCreate, BunchCreateEnd) : 0; Site && !CreateChannel; Site = FindRawCall(Site + 5, BunchCreateEnd))
        {
            const uintptr_t Target = Finder::RelativeTarget(Site, 1, 5);
            if (NamedCreate)
            {
                // The flags of a channel the other side opened are EChannelCreateFlags::None (1), not a cleared r8d
                // (mov edi, 1; mov r8d, edi), so the by-name function is known by the "No free channel" log it holds.
                CreateChannel = NoFreeChannel && Target == NoFreeChannel ? Target : 0;
                continue;
            }
            // CreateChannel looks for a free index below MAX_CHANNELS: 10240 (0x2800) up to 2.1, 32767 (0x7FFF) on 2.2.
            const bool bMaxChannels = FindInstruction(Target, Target + 0x80, { 0x00, 0x28, 0x00, 0x00 }) || FindInstruction(Target, Target + 0x80, { 0xFF, 0x7F, 0x00, 0x00 }) || (NoFreeChannel && Target == NoFreeChannel);
            if (FindInstruction(Site - 0x10, Site, { 0x45, 0x33, 0xC0 }) && bMaxChannels)
            {
                CreateChannel = Target;
            }
        }
        if (NamedCreate)
        {
            Require(L"UNetConnection::CreateChannelByName", Natives::UNetConnection_CreateChannelByName, CreateChannel);
        }
        else
        {
            Require(L"UNetConnection::CreateChannel", Natives::UNetConnection_CreateChannel, CreateChannel);
        }

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
        // The last call that loads the driver's object list into rcx. 2.5 (UE 4.20, 0x21107D0) adds one more call after
        // it, RenamedStartupActors.Remove(Actor's name) on a TMap the driver holds inline (lea rcx, [r14+288h]). 3.3
        // (0x21F2B62) loads the actor after the list: mov rcx, [rdi+508h]; mov rdx, rbx; call. Without it the last
        // match was FNetworkObjectList::Find (0x2212AA0, which returns its TSharedPtr through rdx), and every actor
        // ServerReplicateActors_BuildConsiderList removed had its vtable zeroed (a missed Find writes an empty pointer):
        // the server crashed in UNetConnection::GetTimeoutValue on the connection's OwningActor as a player joined (#244).
        // 5.0 holds the actor in r15: mov rcx, [rdi+550h]; mov rdx, r15 (49 8B D7; CL-4214610 0x24E8087), taken only when
        // no call matches the older way.
        for (const uint8_t ActorRexMask : { static_cast<uint8_t>(0xFF), static_cast<uint8_t>(0xFE) })
        {
            for (auto Call = NotifyCalls.rbegin(); Call != NotifyCalls.rend() && !ObjectListRemove; ++Call)
            {
                const bool bActorLoad = (Byte(Call->first - 3) & ActorRexMask) == 0x48 && Byte(Call->first - 2) == 0x8B && (Byte(Call->first - 1) & 0xF8) == 0xD0;
                const uintptr_t Load = Call->first - (bActorLoad ? 10 : 7);
                if ((Byte(Load) & 0xFE) == 0x48 && Byte(Load + 1) == 0x8B && (Byte(Load + 2) & 0xF8) == 0x88 && (Byte(Load + 2) & 0x07) != 0x04)
                {
                    ObjectListRemove = Call->second;
                    NetworkObjects = *reinterpret_cast<const int32_t*>(Load + 3);
                }
            }
        }
        Require(L"FNetworkObjectList::Remove", Natives::FNetworkObjectList_Remove, ObjectListRemove);
        RequireOffset(L"UNetDriver::NetworkObjects", Natives::UNetDriver_NetworkObjects, NetworkObjects);

        // FNetworkObjectInfo's size: MarkActive (the "no longer dormant on all connections" log) first calls FindOrAdd,
        // which allocates a new one with mov ecx, size; call (1.7.2 0x22270A0: 0xC8; 1.8 0x229C080: 0xD0). 1.8's extra
        // 8 bytes are a TWeakObjectPtr<AActor> after Actor (its constructor 0x2292660), which moves every later member;
        // reading 1.7.2's layout there crashed a 1.8 server in IsActorDormant as soon as a player joined.
        const uintptr_t MarkActive = FunctionOf(L"FNetworkObjectList::MarkDormant: Actor is no longer dormant on all connections. Actor: %s. Total: %i, Active: %i, Connection: %s");
        const uintptr_t FindOrAdd = MarkActive ? Finder::NextCall(MarkActive, 0x60) : 0;
        const uintptr_t FindOrAddEnd = FindOrAdd ? Finder::FunctionEnd(FindOrAdd) : 0;
        const uintptr_t Allocate = FindOrAddEnd ? FindInstruction(FindOrAdd, FindOrAddEnd, { 0xB9, -1, 0x00, 0x00, 0x00, 0xE8 }) : 0;
        const int32_t InfoSize = Allocate ? *reinterpret_cast<const int32_t*>(Allocate + 1) : 0;
        if (InfoSize == 0xD0)
        {
            for (ptrdiff_t* Offset : { &Natives::FNetworkObjectInfo_NextUpdateTime, &Natives::FNetworkObjectInfo_LastNetReplicateTime, &Natives::FNetworkObjectInfo_OptimalNetUpdateDelta,
                                       &Natives::FNetworkObjectInfo_LastNetUpdateTime, &Natives::FNetworkObjectInfo_Flags, &Natives::FNetworkObjectInfo_DormantConnections,
                                       &Natives::FNetworkObjectInfo_RecentlyDormantConnections })
            {
                *Offset += 8;
            }
        }
        else if (InfoSize != 0xC8)
        {
            Log::Warning(L"Finder: FNetworkObjectInfo's size not found (0x{:X}); assuming 1.7.2's layout", InfoSize);
        }
        Log::Info(L"Finder: FNetworkObjectInfo is 0x{:X} bytes, DormantConnections +0x{:X}", InfoSize, Natives::FNetworkObjectInfo_DormantConnections);

        ptrdiff_t ActorChannels = 0;
        int32_t ActorChannelLookups = 0;
        for (uintptr_t Load = NotifyDestroyed; NotifyDestroyedEnd && Load + 12 <= NotifyDestroyedEnd; ++Load)
        {
            // sub eax, [reg+X+0x2C] up to 2.4.2; 2.5 (0x211094D) compares instead: cmp eax, [reg+X+0x2C].
            if (Byte(Load) == 0x8B && (Byte(Load + 1) & 0xC0) == 0x80 && (Byte(Load + 1) & 0x07) != 0x04 && (Byte(Load + 6) == 0x2B || Byte(Load + 6) == 0x3B) && (Byte(Load + 7) & 0xC0) == 0x80 && (Byte(Load + 7) & 0x07) != 0x04
                && *reinterpret_cast<const int32_t*>(Load + 8) - *reinterpret_cast<const int32_t*>(Load + 2) == 0x2C)
            {
                ActorChannels = *reinterpret_cast<const int32_t*>(Load + 2) - 8;
                ++ActorChannelLookups;
            }
        }
        // 5.0 holds the connection in r13, so both carry a REX prefix: mov eax, [r13+1610h]; sub eax, [r13+163Ch]
        // (CL-4214610 0x24E7ECA).
        for (uintptr_t Load = NotifyDestroyed; ActorChannelLookups == 0 && NotifyDestroyedEnd && Load + 14 <= NotifyDestroyedEnd; ++Load)
        {
            if (Byte(Load) == 0x41 && Byte(Load + 1) == 0x8B && (Byte(Load + 2) & 0xC0) == 0x80 && (Byte(Load + 2) & 0x07) != 0x04 && Byte(Load + 7) == 0x41 && (Byte(Load + 8) == 0x2B || Byte(Load + 8) == 0x3B)
                && (Byte(Load + 9) & 0xC0) == 0x80 && (Byte(Load + 9) & 0x07) == (Byte(Load + 2) & 0x07) && *reinterpret_cast<const int32_t*>(Load + 10) - *reinterpret_cast<const int32_t*>(Load + 3) == 0x2C)
            {
                ActorChannels = *reinterpret_cast<const int32_t*>(Load + 3) - 8;
                ++ActorChannelLookups;
            }
        }
        RequireOffset(L"UNetConnection::ActorChannels", Natives::UNetConnection_ActorChannels, ActorChannelLookups == 1 ? ActorChannels : 0);

        uintptr_t CreateDestructionInfo = 0;
        ptrdiff_t ConnectionDestroyedActors = 0;
        ptrdiff_t DestructionInfoNetGUID = 0;
        int32_t DestroyedActorAdds = 0;
        // 5.0's connection is in r13: lea rcx, [r13+1660h] (49 8D 8D; CL-4214610 0x24E801E), tried when rbx..rdi find none.
        for (const int32_t SetRex : { 0x48, 0x49 })
        {
            for (const auto& [Site, Target] : DestroyedActorAdds == 0 ? NotifyCalls : std::vector<std::pair<uintptr_t, uintptr_t>>())
            {
                // Connection->DestroyedStartupOrDormantActors.Add(DestructionInfo->NetGUID): lea rcx, [reg+Set] right after
                // the call up to 3.2. 3.3 (0x21F2ADA) first hands the info to the connection's ReplicationConnectionDriver
                // when it has one, so the Add is past a branch: lea r8, [rax+NetGUID]; xor r9d, r9d; lea rcx, [rbx+1648h].
                // 5.0's Add call ends 0x42 past the CreateDestructionInfo call (0x24E8029).
                const uintptr_t NextCall = FindRawCall(Site + 5, Site + (SetRex == 0x49 ? 0x50 : 0x40));
                const uintptr_t GuidLea = NextCall ? FindInstruction(Site + 5, NextCall, { 0x4C, 0x8D, 0x40 }) : 0;
                uintptr_t Lea = 0;
                for (uintptr_t Candidate = GuidLea ? FindInstruction(Site + 5, NextCall, { SetRex, 0x8D }) : 0; Candidate && !Lea; Candidate = FindInstruction(Candidate + 1, NextCall, { SetRex, 0x8D }))
                {
                    if ((Byte(Candidate + 2) & 0xF8) == 0x88 && (Byte(Candidate + 2) & 0x07) != 0x04)
                    {
                        Lea = Candidate;
                    }
                }
                if (Lea)
                {
                    CreateDestructionInfo = Target;
                    ConnectionDestroyedActors = *reinterpret_cast<const int32_t*>(Lea + 3);
                    DestructionInfoNetGUID = Byte(GuidLea + 3);
                    ++DestroyedActorAdds;
                }
            }
        }
        RequireOffset(L"UNetConnection::DestroyedStartupOrDormantActors", Natives::UNetConnection_DestroyedStartupOrDormantActors, DestroyedActorAdds == 1 ? ConnectionDestroyedActors : 0);

        // The connection's set gets the new destruction info's NetGUID: lea r8, [rax+NetGUID] (1.9.1 0x22E4E99: +0x14;
        // 1.10 0x1ACBF49: +0x1C). 1.10's extra 8 bytes are a TWeakObjectPtr<ULevel> first (CreateDestructionInfo
        // 0x1ABE260 fills Level, ObjOuter, DestroyedPosition +0x10, NetGUID +0x1C, PathName +0x20, StreamingLevelName +0x30
        // in 0x48-byte map elements). Reading 4.16's layout there missed every lookup and crashed a 1.10 server in
        // FActorPriority once a startup actor was destroyed near a player (#160).
        if (DestroyedActorAdds == 1 && DestructionInfoNetGUID == 0x1C)
        {
            for (ptrdiff_t* Offset : { &Natives::FActorDestructionInfo_ObjOuter, &Natives::FActorDestructionInfo_DestroyedPosition, &Natives::FActorDestructionInfo_NetGUID,
                                       &Natives::FActorDestructionInfo_PathName, &Natives::FActorDestructionInfo_StreamingLevelName })
            {
                *Offset += 8;
            }
            Natives::FActorDestructionInfo_Size += 8;
        }
        else if (DestroyedActorAdds == 1 && DestructionInfoNetGUID != 0x14)
        {
            Log::Warning(L"Finder: FActorDestructionInfo's NetGUID found at +0x{:X}; assuming 1.9.1's layout", DestructionInfoNetGUID);
        }
        Log::Info(L"Finder: FActorDestructionInfo is 0x{:X} bytes, NetGUID +0x{:X}", Natives::FActorDestructionInfo_Size, Natives::FActorDestructionInfo_NetGUID);

        // DormantReplicatorMap sits between ActorChannels and DestroyedStartupOrDormantActors up to 3.2 (0x1690, set at
        // 0x16E0); 3.3 (UE 4.20's order) puts it after the set (0x16E8, set at 0x1648).
        ptrdiff_t DormantReplicators = 0;
        int32_t DormantReplicatorRemoves = 0;
        for (uintptr_t Lea = NotifyDestroyed; ActorChannels > 0 && DestroyedActorAdds == 1 && Lea + 7 <= NotifyDestroyedEnd; ++Lea)
        {
            const ptrdiff_t Offset = *reinterpret_cast<const int32_t*>(Lea + 3);
            if (Byte(Lea) == 0x48 && Byte(Lea + 1) == 0x8D && (Byte(Lea + 2) & 0xF8) == 0x88 && (Byte(Lea + 2) & 0x07) != 0x04 && Offset > ActorChannels && Offset != ConnectionDestroyedActors && Offset < ConnectionDestroyedActors + 0x100)
            {
                DormantReplicators = Offset;
                ++DormantReplicatorRemoves;
            }
        }
        // 4.4 (0x227DA30) moves the removal into a connection helper it calls per connection (0x2271BC0, as 4.21's
        // UNetConnection::CleanupDormantReplicatorsForActor): DormantReplicatorMap.Remove for the actor and each of its
        // subobjects, both lea rcx, [rbp+1700h]. Count the distinct offsets in the callees.
        if (DormantReplicatorRemoves == 0 && ActorChannels > 0 && DestroyedActorAdds == 1)
        {
            std::vector<uintptr_t> Scanned;
            std::vector<ptrdiff_t> Found;
            for (const auto& [Site, Target] : NotifyCalls)
            {
                if (std::find(Scanned.begin(), Scanned.end(), Target) != Scanned.end())
                {
                    continue;
                }
                Scanned.push_back(Target);
                const uintptr_t TargetEnd = Finder::FunctionEnd(Target);
                for (uintptr_t Lea = Target; TargetEnd && TargetEnd - Target < 0x200 && Lea + 7 <= TargetEnd; ++Lea)
                {
                    const ptrdiff_t Offset = *reinterpret_cast<const int32_t*>(Lea + 3);
                    if (Byte(Lea) == 0x48 && Byte(Lea + 1) == 0x8D && (Byte(Lea + 2) & 0xF8) == 0x88 && (Byte(Lea + 2) & 0x07) != 0x04 && Offset > ActorChannels && Offset != ConnectionDestroyedActors && Offset < ConnectionDestroyedActors + 0x100
                        && std::find(Found.begin(), Found.end(), Offset) == Found.end())
                    {
                        Found.push_back(Offset);
                    }
                }
            }
            if (Found.size() == 1)
            {
                DormantReplicators = Found[0];
                DormantReplicatorRemoves = 1;
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
        ptrdiff_t ChannelBits = ClosingTest ? Byte(ClosingTest + 2) : 0;
        if (Close && !ClosingTest)
        {
            // 4.2 (0x2054D20) loads the bits once for two tests: mov ecx, [rcx+30h]; test cl, 2; ...; bt ecx, 9.
            const uintptr_t Load = FindInstruction(Close, Close + 0x20, { 0x8B, -1, -1, 0xF6, -1, 0x02 });
            const uint8_t Register = Load ? (Byte(Load + 1) >> 3) & 0x07 : 0;
            ChannelBits = Load && (Byte(Load + 1) & 0xC7) == 0x41 && Byte(Load + 4) == (0xC0 | Register) ? Byte(Load + 2) : 0;
        }
        RequireOffset(L"UChannel::Bits", Natives::UChannel_Bits, ChannelBits);

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

        // FBitWriter's overflow report (Max - Num, then ArIsError's bit set). Up to 6.10 the string has one user, the
        // writer's out-of-line overflow function (6.10 0x1731B90: mov rax, [rcx+Max]; sub r8, [rcx+Num]). 6.20 also
        // inlines the report into FBitWriter::SerializeIntPacked (0x17855E0, whose Max - Num is register to register),
        // and that copy is the first reference, so each function holding the string is read in turn, the first first.
        ptrdiff_t ErrorByte = 0;
        uint32_t ErrorMask = 0;
        ptrdiff_t WriterNum = 0;
        const std::initializer_list<int32_t> SubtractNum = { -1, 0x2B, -1, -1, -1, -1, -1 };
        const std::initializer_list<int32_t> LoadMax = { 0x48, 0x8B, -1, -1, -1, -1, -1 };
        for (const uintptr_t OverflowRef : Finder::StringRefs(L"FBitWriter overflowed! (WriteLen: %i, Remaining: %i, Max: %i)"))
        {
            const uintptr_t Overflowed = Finder::FunctionStart(OverflowRef);
            const uintptr_t OverflowedEnd = Overflowed ? Finder::FunctionEnd(Overflowed) : 0;
            for (uintptr_t Or = OverflowedEnd && !ErrorMask ? FindInstruction(Overflowed, OverflowedEnd, { 0x80, -1, -1, -1 }) : 0; Or && !ErrorMask; Or = FindInstruction(Or + 1, OverflowedEnd, { 0x80, -1, -1, -1 }))
            {
                if ((Byte(Or + 1) & 0xF8) == 0x48 && (Byte(Or + 1) & 0x07) != 0x04 && Byte(Or + 3) && (Byte(Or + 3) & (Byte(Or + 3) - 1)) == 0)
                {
                    ErrorByte = Byte(Or + 2);
                    ErrorMask = Byte(Or + 3);
                }
            }
            for (uintptr_t Sub = OverflowedEnd && !WriterNum ? FindInstruction(Overflowed, OverflowedEnd, SubtractNum) : 0; Sub && !WriterNum; Sub = FindInstruction(Sub + 1, OverflowedEnd, SubtractNum))
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
            if (ErrorMask && WriterNum)
            {
                break;
            }
        }
        RequireOffset(L"FArchive::ArIsError", Natives::FArchive_ArIsError, ErrorByte);
        Natives::FArchive_ArIsErrorMask = ErrorMask;
        RequireOffset(L"FBitWriter::Num", Natives::FBitWriter_Num, WriterNum);
        Log::Info(L"Finder: FOutBunch is 0x{:X} bytes, bClose +0x{:X} & 0x{:X}, bDormant +0x{:X} & 0x{:X}, bReliable +0x{:X} & 0x{:X}, Num +0x{:X}, ArIsError +0x{:X} & 0x{:X}, UChannel::ChIndex +0x{:X}", BunchSize, BunchClose, CloseMask, BunchDormant, DormantMask, BunchReliable, ReliableMask, WriterNum, ErrorByte, ErrorMask, ChIndex);

        ptrdiff_t NumOutRec = 0;
        // 3.3 (0x2074B10) times the replication first (QueryPerformanceCounter), which moves the test to +0xB9.
        const uintptr_t PausedTest = ReplicateActor ? FindInstruction(ReplicateActor, ReplicateActor + 0x100, { 0x0F, 0xBA, -1, 0x08 }) : 0;
        for (uintptr_t Compare = PausedTest && (Byte(PausedTest + 2) & 0x38) == 0x20 ? FindInstruction(PausedTest + 4, PausedTest + 0x14, { 0x83, -1, -1, 0x00 }) : 0; Compare && !NumOutRec; Compare = FindInstruction(Compare + 1, PausedTest + 0x14, { 0x83, -1, -1, 0x00 }))
        {
            if ((Byte(Compare + 1) & 0xF8) == 0x78 && (Byte(Compare + 1) & 0x07) != 0x04)
            {
                NumOutRec = Byte(Compare + 2);
            }
        }
        // 5.0 (UE 4.21) compares with a zeroed register, the channel in r12: cmp [r12+4Ch], r15d (45 39 7C 24 4C;
        // CL-4214610 0x2350970).
        for (uintptr_t Compare = PausedTest && !NumOutRec && (Byte(PausedTest + 2) & 0x38) == 0x20 ? PausedTest + 4 : 0; Compare && Compare < PausedTest + 0x14 && !NumOutRec; ++Compare)
        {
            const uint8_t ModRm = Byte(Compare + 1);
            if (Byte(Compare) != 0x39 || (Byte(Compare - 1) & 0xF0) != 0x40 || (ModRm & 0xC0) != 0x40)
            {
                continue;
            }
            const bool bSib = (ModRm & 0x07) == 0x04;
            if (!bSib || Byte(Compare + 2) == 0x24)
            {
                NumOutRec = Byte(Compare + (bSib ? 3 : 2));
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
        // 5.0 holds the channel in r12, which takes a SIB byte: movsd [r12+80h], xmm0 (F2 41 0F 11 84 24; CL-4214610
        // 0x2351949).
        for (uintptr_t Store = LastUpdateStores == 0 && ReplicateActorEnd ? FindInstruction(ReplicateActor, ReplicateActorEnd, { 0x0F, 0x5A, 0xC0, 0xF2, 0x41, 0x0F, 0x11, 0x84, 0x24 }) : 0; Store; Store = FindInstruction(Store + 1, ReplicateActorEnd, { 0x0F, 0x5A, 0xC0, 0xF2, 0x41, 0x0F, 0x11, 0x84, 0x24 }))
        {
            LastUpdateTime = *reinterpret_cast<const int32_t*>(Store + 9);
            ++LastUpdateStores;
        }
        // 3.0 moved LastUpdateTime off the channel (UActorChannel lost a double before its bits) into a map on the
        // connection, TMap<TWeakObjectPtr<AActor>, double> at +0x1860: ReplicateActor (3.0 0x1FDAE90) finds or adds the
        // actor's entry (lea rcx, [conn+map]; call FindId / Emplace), loads the map's data (mov rax, [conn+map]) and
        // stores Driver->Time into the value (movsd [rdx], xmm0). RelevantTime is still the double before the bits. 3.1
        // put it back on the channel (0x1FEF480: movsd [r13 + 80h], xmm0, RelevantTime +0x78), the store found above.
        ptrdiff_t LastUpdateTimes = 0;
        int32_t LastUpdateTimesStores = 0;
        const std::initializer_list<int32_t> MapTimeStore = { 0x0F, 0x5A, 0xC0, 0xF2, 0x0F, 0x11, -1 };
        for (uintptr_t Store = LastUpdateStores == 0 && ReplicateActorEnd ? FindInstruction(ReplicateActor, ReplicateActorEnd, MapTimeStore) : 0; Store; Store = FindInstruction(Store + 1, ReplicateActorEnd, MapTimeStore))
        {
            if ((Byte(Store + 6) & 0xC0) != 0x00 || (Byte(Store + 6) & 0x07) == 0x04 || (Byte(Store + 6) & 0x07) == 0x05)
            {
                continue;
            }
            ptrdiff_t MapOffset = 0;
            const std::initializer_list<int32_t> LoadData = { 0x48, 0x8B, -1, -1, -1, 0x00, 0x00 };
            for (uintptr_t Load = FindInstruction(Store - 0x30, Store, LoadData); Load; Load = FindInstruction(Load + 1, Store, LoadData))
            {
                if ((Byte(Load + 2) & 0xF8) == 0x80 && (Byte(Load + 2) & 0x07) != 0x04)
                {
                    MapOffset = *reinterpret_cast<const int32_t*>(Load + 3);
                }
            }
            const uintptr_t MapLea = MapOffset > 0 ? FindInstruction(Store - 0x90, Store, { 0x48, 0x8D, -1, static_cast<int32_t>(MapOffset & 0xFF), static_cast<int32_t>((MapOffset >> 8) & 0xFF), 0x00, 0x00 }) : 0;
            if (MapLea && (Byte(MapLea + 2) & 0xF8) == 0x88)
            {
                LastUpdateTimes = MapOffset;
                ++LastUpdateTimesStores;
            }
        }
        if (LastUpdateStores == 0 && LastUpdateTimesStores == 1)
        {
            RequireOffset(L"UNetConnection::LastUpdateTimes", Natives::UNetConnection_LastUpdateTimes, LastUpdateTimes);
            RequireOffset(L"UActorChannel::RelevantTime", Natives::UActorChannel_RelevantTime, Natives::UActorChannel_Bits > 0 ? Natives::UActorChannel_Bits - static_cast<ptrdiff_t>(sizeof(double)) : 0);
            Log::Info(L"Finder: UActorChannel::LastUpdateTime is the connection's map at +0x{:X}, RelevantTime +0x{:X}", LastUpdateTimes, Natives::UActorChannel_RelevantTime);
        }
        else
        {
            RequireOffset(L"UActorChannel::LastUpdateTime", Natives::UActorChannel_LastUpdateTime, LastUpdateStores == 1 ? LastUpdateTime : 0);
            RequireOffset(L"UActorChannel::RelevantTime", Natives::UActorChannel_RelevantTime, LastUpdateStores == 1 ? LastUpdateTime - static_cast<ptrdiff_t>(sizeof(double)) : 0);
        }

        uintptr_t WorldSettings = 0;
        int32_t WorldSettingsCalls = 0;
        for (uintptr_t Call = ReplicateActorEnd ? FindInstruction(ReplicateActor, ReplicateActorEnd, { 0x41, 0xB0, 0x01, 0x33, 0xD2, 0x48, 0x8B, 0xC8, 0xE8 }) : 0; Call; Call = FindInstruction(Call + 1, ReplicateActorEnd, { 0x41, 0xB0, 0x01, 0x33, 0xD2, 0x48, 0x8B, 0xC8, 0xE8 }))
        {
            WorldSettings = Finder::RelativeTarget(Call + 8, 1, 5);
            ++WorldSettingsCalls;
        }
        if (!WorldSettingsCalls)
        {
            // 1.10 schedules a stack store between the arguments and passes the world from r14 (0x195319E, mov rcx, r14;
            // call 0x1D442E0): take the first call after mov r8b, 1; xor edx, edx that follows a mov rcx, reg.
            for (uintptr_t Flags = ReplicateActorEnd ? FindInstruction(ReplicateActor, ReplicateActorEnd, { 0x41, 0xB0, 0x01, 0x33, 0xD2 }) : 0; Flags; Flags = FindInstruction(Flags + 1, ReplicateActorEnd, { 0x41, 0xB0, 0x01, 0x33, 0xD2 }))
            {
                const uintptr_t Call = FindRawCall(Flags + 5, Flags + 0x20);
                if (Call && (Byte(Call - 3) & 0xFE) == 0x48 && Byte(Call - 2) == 0x8B && (Byte(Call - 1) & 0xF8) == 0xC8)
                {
                    WorldSettings = Finder::RelativeTarget(Call, 1, 5);
                    ++WorldSettingsCalls;
                }
            }
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
        if (!ArchetypeCalls)
        {
            // 2.3 no longer inlines UObject::GetArchetype (0x1B2F526, call 0xDF8110): it checks the archetype cache and
            // only then loads Class, Outer, Name and Flags from the object (rdi here) to call GetArchetypeFromRequiredInfo
            // (0xDF8178, call 0xDF81B0). Take that inner call from whichever SerializeNewActor callee makes it.
            for (uintptr_t Call = FindRawCall(SerializeNewActor, SerializeNewActorEnd); Call; Call = FindRawCall(Call + 1, SerializeNewActorEnd))
            {
                const uintptr_t GetArchetype = Finder::RelativeTarget(Call, 1, 5);
                uintptr_t Load = FindInstruction(GetArchetype, GetArchetype + 0x80, { 0x44, 0x8B, -1, 0x08, 0x4C, 0x8B, -1, 0x18, 0x48, 0x8B, -1, 0x20, 0x48, 0x8B, -1, 0x10, 0xE8 });
                uintptr_t LoadCall = Load ? Load + 16 : 0;
                if (!Load)
                {
                    // 3.5.1 (0x14C39D7) schedules a stack store between the loads and the call: mov [rsp+X], rsi. 4.2
                    // (0x1519267) stores its fifth argument, bUseUpToDateClass, after that one: mov [rsp+20h], bl.
                    Load = FindInstruction(GetArchetype, GetArchetype + 0x80, { 0x44, 0x8B, -1, 0x08, 0x4C, 0x8B, -1, 0x18, 0x48, 0x8B, -1, 0x20, 0x48, 0x8B, -1, 0x10, 0x48, 0x89, -1, 0x24, -1 });
                    LoadCall = Load && (Byte(Load + 18) & 0xC7) == 0x44 ? Load + 21 : 0;
                    if (LoadCall && Byte(LoadCall) == 0x88 && (Byte(LoadCall + 1) & 0xC7) == 0x44 && Byte(LoadCall + 2) == 0x24 && Byte(LoadCall + 3) == 0x20)
                    {
                        LoadCall += 4;
                    }
                    // 5.0 (UE 4.21, 0x17958F5) keeps it in r14b: mov [rsp+20h], r14b (44 88 74 24 20).
                    else if (LoadCall && Byte(LoadCall) == 0x44 && Byte(LoadCall + 1) == 0x88 && (Byte(LoadCall + 2) & 0xC7) == 0x44 && Byte(LoadCall + 3) == 0x24 && Byte(LoadCall + 4) == 0x20)
                    {
                        LoadCall += 5;
                    }
                    LoadCall = LoadCall && Byte(LoadCall) == 0xE8 ? LoadCall : 0;
                }
                // One [base + disp8] for all four loads, the base not rsp.
                const auto Base = [&](uintptr_t Modrm) { return Byte(Modrm) & 0xC7; };
                if (LoadCall && (Base(Load + 2) & 0xC0) == 0x40 && (Base(Load + 2) & 0x07) != 0x04 && Base(Load + 6) == Base(Load + 2) && Base(Load + 10) == Base(Load + 2) && Base(Load + 14) == Base(Load + 2))
                {
                    const uintptr_t Target = Finder::RelativeTarget(LoadCall, 1, 5);
                    if (Target != Archetype)
                    {
                        Archetype = Target;
                        ++ArchetypeCalls;
                    }
                }
            }
        }
        Require(L"UObject::GetArchetypeFromRequiredInfo", Natives::UObject_GetArchetypeFromRequiredInfo, ArchetypeCalls == 1 && IsInText(Archetype) ? Archetype : 0);

        const FCalledSlot NewActor = ReplicateActorEnd ? CalledSlot(ReplicateActor, ReplicateActorEnd, SerializeNewActor) : FCalledSlot();
        const int32_t WriteObject = Finder::VTableIndex(NewActor.VTable, reinterpret_cast<void*>(FunctionOf(L"WroteObject %s NetGUID <%s>")), 0x400);
        RequireSlot(L"UPackageMap::WriteObject", Natives::UPackageMap_WriteObjectIndex, WriteObject >= 0 && WriteObject == NewActor.Index - 4 ? WriteObject : -1);

        const uintptr_t PlayReplay = FunctionOf(L"PlayReplay: failed to create demo net driver!");
        const uintptr_t PlayReplayEnd = Finder::FunctionEnd(PlayReplay);
        const FCalledSlot InitConnect = PlayReplayEnd ? CalledSlot(PlayReplay, PlayReplayEnd, FunctionOf(L"UNetDriver::InitConnect")) : FCalledSlot();
        void** NetDriverVTable = InitConnect.VTable;

        // 2.4.2 moves the real ProcessRemoteFunction into UNetDriver (0x1B16EA0, as in 4.20): the stub's log is gone and
        // its own "no owning connection" warning names UNetDriver.
        uintptr_t DriverRemoteFunction = FunctionOf(L"UNetDriver::ProcessRemoteFunction");
        DriverRemoteFunction = DriverRemoteFunction ? DriverRemoteFunction : FunctionOf(L"UNetDriver::ProcessRemoteFunction: No owning connection for actor %s. Function %s will not be processed.");
        const int32_t ProcessRemoteFunction = Finder::VTableIndex(NetDriverVTable, reinterpret_cast<void*>(DriverRemoteFunction), 0x400);
        const int32_t LowLevelGetNetworkNumber = Finder::VTableIndex(NetDriverVTable, reinterpret_cast<void*>(FunctionOf(L"UNetDriver::LowLevelGetNetworkNumber")), 0x400);
        const int32_t ServerReplicateActors = ProcessRemoteFunction - 1;
        const uintptr_t StrippedReplicate = ProcessRemoteFunction > 0 && ServerReplicateActors == LowLevelGetNetworkNumber + 2 ? VTableEntry(NetDriverVTable, ServerReplicateActors) : 0;
        bool bServerReplicateActors = Finder::BodyIs(StrippedReplicate, { 0x33, 0xC0, 0xC3 });
        if (!bServerReplicateActors && StrippedReplicate)
        {
            // 3.5.1's stub (0x229EB20) keeps the function's SCOPE_CYCLE_COUNTER(STAT_NetServerRepActorsTime) before its
            // return 0: xor eax, eax; add rsp, 28h; ret.
            const uintptr_t StrippedReplicateEnd = Finder::FunctionEnd(StrippedReplicate);
            const std::vector<uintptr_t> StatRefs = Finder::StringRefs(L"STAT_NetServerRepActorsTime");
            const bool bStatScope = std::any_of(StatRefs.begin(), StatRefs.end(), [&](uintptr_t Ref) { return Ref >= StrippedReplicate && Ref < StrippedReplicateEnd; });
            bServerReplicateActors = bStatScope && StrippedReplicateEnd - StrippedReplicate <= 0x60
                && FindInstruction(StrippedReplicate, StrippedReplicateEnd, { 0x33, 0xC0, 0x48, 0x83, 0xC4, -1, 0xC3 });
        }
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
        // TickFlush is the slot after TickDispatch, the one UDemoNetDriver::TickFlush calls (Super::TickFlush). 4.5 adds
        // PostTickDispatch between them (4.21's; CL-4166199 slot +1 0x228ECD0, TickFlush +2 0x22988E0).
        const uintptr_t DemoTickFlush = FunctionOf(L"UDemoNetDriver::TickFlush: ReplayStreamer ERROR: %s");
        int32_t TickFlushIndex = -1;
        uintptr_t TickFlush = 0;
        for (int32_t After = 1; After <= 2 && TickDispatch >= 0 && DemoTickFlush && TickFlushIndex < 0; ++After)
        {
            const uintptr_t Candidate = VTableEntry(NetDriverVTable, TickDispatch + After);
            for (const uintptr_t Site : Candidate ? Finder::CallsTo(Candidate, false) : std::vector<uintptr_t>())
            {
                if (Finder::FunctionStart(Site) == DemoTickFlush)
                {
                    TickFlushIndex = TickDispatch + After;
                    TickFlush = Candidate;
                    break;
                }
            }
        }
        const bool bDemoTickFlush = TickFlushIndex >= 0;
        const uintptr_t TickFlushEnd = Finder::FunctionEnd(TickFlush);
        RequireSlot(L"UNetDriver::TickFlush", Natives::UNetDriver_TickFlushIndex, TickFlushIndex);
        Require(L"UNetDriver::TickFlush", Natives::UNetDriver_TickFlush, bDemoTickFlush ? TickFlush : 0);

        // IsServer: return ServerConnection == nullptr, cmp qword ptr [rcx+ServerConnection], 0; sete al; ret. 5.0 (UE
        // 4.21) moves ServerConnection past 0x7F: cmp qword ptr [rcx+88h], 0 (CL-4214610 0x24E7310).
        const auto IsServerShape = [](uintptr_t Body)
        {
            return Body && (FindInstruction(Body, Body + 9, { 0x48, 0x83, 0x79, -1, 0x00, 0x0F, 0x94, 0xC0, 0xC3 }) == Body
                || FindInstruction(Body, Body + 12, { 0x48, 0x83, 0xB9, -1, -1, 0x00, 0x00, 0x00, 0x0F, 0x94, 0xC0, 0xC3 }) == Body);
        };
        int32_t IsServer = TickFlush ? VirtualSlot(FindVirtualCall(TickFlush, TickFlush + 0x60)) : -1;
        uintptr_t IsServerBody = VTableEntry(NetDriverVTable, IsServer);
        const uintptr_t GetNetMode = reinterpret_cast<uintptr_t>(Natives::UNetDriver_GetNetMode);
        bool bIsServer = IsServerShape(IsServerBody) && GetNetMode && VirtualSlot(FindVirtualCall(GetNetMode, GetNetMode + 0x10)) == IsServer;
        if (!bIsServer && GetNetMode)
        {
            // 1.10's TickFlush (0x1AD79B0) no longer starts with IsServer; GetNetMode (0x1AC2BB0) still does (slot 0x60).
            IsServer = VirtualSlot(FindVirtualCall(GetNetMode, GetNetMode + 0x10));
            IsServerBody = VTableEntry(NetDriverVTable, IsServer);
            bIsServer = IsServerShape(IsServerBody);
        }
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
            // 6.10's by-name call builds its FName(NAME_Actor) between the two (mov qword ptr [rbp+5Fh], 66h; lea rdx,
            // [rbp+5Fh]), so IsLevelInitializedForActor's call is 0x27 before it (0x26F9CA4 and 0x26F9CCB).
            const uintptr_t Window = NamedCreate ? 0x30 : 0x20;
            uintptr_t Nearest = 0;
            for (uintptr_t Call = FindVirtualCall(Site - Window, Site); Call; Call = FindVirtualCall(Call + 1, Site))
            {
                Nearest = Call;
            }
            // 3.3 adds a second caller, the ReplicationGraph's (0x6DF1B0), which calls the same slot (0x6F).
            if (Nearest && (LevelInitializedSites == 0 || VirtualSlot(Nearest) == LevelInitialized))
            {
                LevelInitialized = VirtualSlot(Nearest);
                ++LevelInitializedSites;
            }
            else if (Nearest)
            {
                LevelInitializedSites = -1;
                break;
            }
        }
        const uintptr_t LevelInitializedBody = LevelInitializedSites >= 1 ? VTableEntry(NetDriverVTable, LevelInitialized) : 0;
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
        // IsNetReady's body: test dl, dl; jz; mov eax, [rcx+QueuedBits']; neg eax; mov [rcx+QueuedBits], eax (5.0, UE 4.21,
        // copies rcx into r8 first: mov r8, rcx; CL-4214610 0x24E71A0).
        const auto IsNetReadyBodyOf = [&](int32_t Slot)
        {
            uintptr_t Body = Slot >= 0 ? VTableEntry(ConnectionVTable, Slot) : 0;
            if (Body && Byte(Body) == 0x4C && Byte(Body + 1) == 0x8B && Byte(Body + 2) == 0xC1)
            {
                Body += 3;
            }
            return Body && FindInstruction(Body, Body + 18, { 0x84, 0xD2, 0x74, -1, 0x8B, 0x81, -1, -1, 0x00, 0x00, 0xF7, 0xD8, 0x89, 0x81, -1, -1, 0x00, 0x00 }) == Body ? Body : 0;
        };
        // 5.10 (CL-4240749) declares one more virtual between them (slot 0x57, an FString getter), so InitRemoteConnection
        // is 0x5A and IsNetReady stays 0x53, seven before it.
        int32_t IsNetReady = InitRemoteConnection - 6;
        if (InitRemoteConnection >= 7 && !IsNetReadyBodyOf(IsNetReady) && IsNetReadyBodyOf(InitRemoteConnection - 7))
        {
            IsNetReady = InitRemoteConnection - 7;
        }
        const int32_t ConnectionTick = IsNetReady - 1;
        int32_t ConnectionTicks = 0;
        for (uintptr_t Call = TickFlushEnd && ConnectionTick >= 0 ? FindInstruction(TickFlush, TickFlushEnd, { 0x48, 0x8B, 0x01, 0xFF, 0x90 }) : 0; Call; Call = FindInstruction(Call + 1, TickFlushEnd, { 0x48, 0x8B, 0x01, 0xFF, 0x90 }))
        {
            ConnectionTicks += VirtualSlot(Call + 3) == ConnectionTick ? 1 : 0;
        }
        const uintptr_t ConnectionTickBody = ConnectionTicks >= 2 ? VTableEntry(ConnectionVTable, ConnectionTick) : 0;
        RequireSlot(L"UNetConnection::Tick", Natives::UNetConnection_TickIndex, ConnectionTickBody ? ConnectionTick : -1);

        const uintptr_t IsNetReadyBody = InitRemoteConnection >= 6 ? IsNetReadyBodyOf(IsNetReady) : 0;
        const bool bIsNetReady = IsNetReadyBody != 0;
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

        // 1.8 caches the answer per outer (a TMap at +0x33738) and tail-jumps to UpdateCachedLevelVisibility (0x22B46D0),
        // which does the TSet lookup; there ClientVisibleLevelNames is +0x336A8, before ClientWorldPackageName (+0x33788).
        // Take the set from its Num / NumFreeIndices pair (+0x08 / +0x34) and Elements load (+0x00) in a function
        // ClientHasInitializedLevelFor jumps to that also reads ClientWorldPackageName.
        ptrdiff_t VisibleLevelSetOffset = 0;
        const int32_t PackageName = static_cast<int32_t>(ClientWorldPackageName);
        for (uintptr_t Jump = !bVisibleLevels && ClientWorldPackageName && LevelForEnd ? FindInstruction(LevelFor, LevelForEnd, { 0xE9 }) : 0; Jump && !VisibleLevelSetOffset; Jump = FindInstruction(Jump + 1, LevelForEnd, { 0xE9 }))
        {
            const uintptr_t Target = Finder::RelativeTarget(Jump, 1, 5);
            const uintptr_t TargetEnd = IsInText(Target) && Finder::FunctionStart(Target) == Target ? Finder::FunctionEnd(Target) : 0;
            if (!TargetEnd || !FindInstruction(Target, TargetEnd, { -1, 0x8B, -1, PackageName & 0xFF, (PackageName >> 8) & 0xFF, (PackageName >> 16) & 0xFF, (PackageName >> 24) & 0xFF }))
            {
                continue;
            }
            for (uintptr_t Count = Target; Count + 6 <= TargetEnd && !VisibleLevelSetOffset; ++Count)
            {
                const uint8_t ModRm = Byte(Count + 1);
                if (Byte(Count) != 0x8B || (ModRm & 0xC0) != 0x80 || (ModRm & 0x07) == 0x04)
                {
                    continue;
                }
                const int32_t Set = *reinterpret_cast<const int32_t*>(Count + 2) - 0x08;
                const int32_t Free = Set + 0x34;
                const uintptr_t FreeEnd = std::min(Count + 0x18, TargetEnd);
                const bool bFree = FindInstruction(Count + 6, FreeEnd, { 0x3B, ModRm, Free & 0xFF, (Free >> 8) & 0xFF, (Free >> 16) & 0xFF, (Free >> 24) & 0xFF })
                    || FindInstruction(Count + 6, FreeEnd, { 0x2B, ModRm, Free & 0xFF, (Free >> 8) & 0xFF, (Free >> 16) & 0xFF, (Free >> 24) & 0xFF });
                bool bElements = false;
                for (uintptr_t Load = bFree ? FindInstruction(Target, TargetEnd, { -1, 0x8B, -1, Set & 0xFF, (Set >> 8) & 0xFF, (Set >> 16) & 0xFF, (Set >> 24) & 0xFF }) : 0; Load && !bElements; Load = FindInstruction(Load + 1, TargetEnd, { -1, 0x8B, -1, Set & 0xFF, (Set >> 8) & 0xFF, (Set >> 16) & 0xFF, (Set >> 24) & 0xFF }))
                {
                    bElements = (Byte(Load) & 0xF8) == 0x48 && (Byte(Load + 2) & 0xC0) == 0x80 && (Byte(Load + 2) & 0x07) != 0x04;
                }
                VisibleLevelSetOffset = bFree && bElements ? Set : 0;
            }
        }
        const bool bVisibleLevelsFound = bVisibleLevels || VisibleLevelSetOffset;
        Natives::UNetConnection_ClientVisibleLevelNamesIsSet = bVisibleLevels ? !bVisibleLevelCount : VisibleLevelSetOffset != 0;
        RequireSlot(L"UNetConnection::ClientHasInitializedLevelFor", Natives::UNetConnection_ClientHasInitializedLevelForIndex, bVisibleLevelsFound ? ClientHasInitializedLevelFor : -1);
        RequireOffset(L"UNetConnection::ClientVisibleLevelNames", Natives::UNetConnection_ClientVisibleLevelNames, bVisibleLevels ? VisibleLevelNames : VisibleLevelSetOffset);

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
        // Only the ahead prediction reads TickCount (and ServerReplicateActors, which Core restores, counts it up): a
        // viewer without it needs no TickCount.
        if (bNetViewerPredicts)
        {
            RequireOffset(L"UNetConnection::TickCount", Natives::UNetConnection_TickCount, TickCountTests == 1 ? *reinterpret_cast<const int32_t*>(TickCountTest + 2) : 0);
        }

        // CreateDestructionInfo loads GuidCache into rcx for AssignNewNetGUID_Server, then takes the driver's map:
        // mov rcx, [rcx+GuidCache]; call; add rbx, Map up to 2.4.2 (0x1B0C9F0); 2.5 (UE 4.20, 0x2105A20) is a static
        // with the driver in rdx: mov rcx, [rsi+GuidCache], four stores, call (+0x1E); lea rbx, [rsi+Map].
        uintptr_t GuidCacheLoad = 0;
        for (uintptr_t Load = CreateDestructionInfo; Load && Load < CreateDestructionInfo + 0x48 && !GuidCacheLoad; ++Load)
        {
            if (Byte(Load) == 0x48 && Byte(Load + 1) == 0x8B && (Byte(Load + 2) & 0xF8) == 0x88 && (Byte(Load + 2) & 0x07) != 0x04)
            {
                GuidCacheLoad = Load;
            }
        }
        const uintptr_t AssignGuid = GuidCacheLoad ? FindRawCall(GuidCacheLoad + 7, GuidCacheLoad + 0x30) : 0;
        ptrdiff_t DestroyedActors = 0;
        uintptr_t DestroyedActorsBase = 0;
        for (uintptr_t Base = AssignGuid ? AssignGuid + 5 : 0; Base && Base < AssignGuid + 0x10 && !DestroyedActors; ++Base)
        {
            const bool bAdd = Byte(Base) == 0x48 && Byte(Base + 1) == 0x81 && (Byte(Base + 2) & 0xF8) == 0xC0;
            const bool bLea = Byte(Base) == 0x48 && Byte(Base + 1) == 0x8D && (Byte(Base + 2) & 0xC0) == 0x80 && (Byte(Base + 2) & 0x07) != 0x04 && (Byte(Base + 2) & 0x07) == (Byte(GuidCacheLoad + 2) & 0x07);
            DestroyedActors = bAdd || bLea ? *reinterpret_cast<const int32_t*>(Base + 3) : 0;
            DestroyedActorsBase = DestroyedActors ? Base : 0;
        }
        // 3.5.1's AssignNewNetGUID_Server (0x228B0B4) returns the NetGUID through rdx, and the invalid-NetGUID warning
        // comes before the map: lea rbx, [rsi+240h] (0x228B143).
        for (uintptr_t Base = AssignGuid && !DestroyedActors ? AssignGuid + 5 : 0; Base && Base < AssignGuid + 0xA0 && !DestroyedActors; ++Base)
        {
            const bool bLea = Byte(Base) == 0x48 && Byte(Base + 1) == 0x8D && (Byte(Base + 2) & 0xC0) == 0x80 && (Byte(Base + 2) & 0x07) != 0x04 && (Byte(Base + 2) & 0x07) == (Byte(GuidCacheLoad + 2) & 0x07);
            DestroyedActors = bLea ? *reinterpret_cast<const int32_t*>(Base + 3) : 0;
            DestroyedActorsBase = DestroyedActors ? Base : 0;
        }
        RequireOffset(L"UNetDriver::GuidCache", Natives::UNetDriver_GuidCache, GuidCacheLoad ? *reinterpret_cast<const int32_t*>(GuidCacheLoad + 3) : 0);
        RequireOffset(L"UNetDriver::DestroyedStartupOrDormantActors", Natives::UNetDriver_DestroyedStartupOrDormantActors, DestroyedActors);

        // The map's FindOrAdd steps through its elements (key, value, then the hash's next id): lea r8, [rdx+rdx*N];
        // cmp [r9+r8*8], ecx; jz; mov eax, [r9+r8*8+HashNextId]. 3.2 (0x2190B90) holds the info in place (+0x40, a 0x38
        // byte value); 3.3 (0x21E7FF0, UE 4.20's TMap<FNetworkGUID, TUniquePtr<FActorDestructionInfo>>) a pointer (+0x10)
        // to one it allocates (mov ecx, 38h). Builds without this lookup keep the info in place. The lookup follows the
        // map's address: 3.5.1's (0x228B143) comes after the invalid-NetGUID warning, whose calls (0x228B0DB, 0x228B11A)
        // are the first after AssignNewNetGUID_Server, so a search from the GUID's call to the first call after it found
        // nothing and the TUniquePtr map (0x18-byte elements, the next id at +0x10, 0x228B19E) was read as 0x48-byte ones:
        // a joining client's destroyed startup actors (UNetDriver::AddClientConnection copies their GUIDs) were looked up
        // at the wrong stride when the server first replicated to it (#262).
        const uintptr_t FindOrAddCall = DestroyedActorsBase ? FindRawCall(DestroyedActorsBase, DestroyedActorsBase + 0xC0) : 0;
        ptrdiff_t DestroyedActorValueSize = 0;
        for (uintptr_t Next = FindOrAddCall ? FindInstruction(DestroyedActorsBase, FindOrAddCall, { 0x8B, 0x44 }) : 0; Next && !DestroyedActorValueSize; Next = FindInstruction(Next + 1, FindOrAddCall, { 0x8B, 0x44 }))
        {
            if ((Byte(Next - 1) & 0xF0) == 0x40 && (Byte(Next + 2) & 0xC0) == 0xC0 && Byte(Next - 3) == 0x74 && Byte(Next - 6) == 0x39 && (Byte(Next - 5) & 0xC7) == 0x04 && (Byte(Next - 4) & 0xC0) == 0xC0)
            {
                DestroyedActorValueSize = static_cast<ptrdiff_t>(Byte(Next + 3)) - 8;
            }
        }
        // UE 4.21 takes the element's address before the jump and reads the next id through it: lea r8, [rdx+rdx*2];
        // cmp [r9+r8*8], ecx; lea rdx, [r9+r8*8]; jz; mov eax, [rdx+10h] (5.0 CL-4204761 0x24DC797). The probe above
        // found nothing there, so 5.0's TUniquePtr map (0x18-byte elements) was read as 0x38-byte values and a client
        // joining a zone with a destroyed startup actor crashed the server in UNetDriver::FindDestructionInfo.
        for (uintptr_t Next = FindOrAddCall && !DestroyedActorValueSize ? DestroyedActorsBase + 10 : 0; Next && Next + 3 <= FindOrAddCall && !DestroyedActorValueSize; ++Next)
        {
            if (Byte(Next) == 0x8B && (Byte(Next + 1) & 0xC0) == 0x40 && (Byte(Next + 1) & 0x07) != 0x04 && Byte(Next - 2) == 0x74
                && (Byte(Next - 6) & 0xF0) == 0x40 && Byte(Next - 5) == 0x8D && (Byte(Next - 4) & 0xC7) == 0x04 && (Byte(Next - 3) & 0xC0) == 0xC0 && ((Byte(Next - 4) >> 3) & 0x07) == (Byte(Next + 1) & 0x07)
                && (Byte(Next - 10) & 0xF0) == 0x40 && Byte(Next - 9) == 0x39 && (Byte(Next - 8) & 0xC7) == 0x04 && Byte(Next - 7) == Byte(Next - 3))
            {
                DestroyedActorValueSize = static_cast<ptrdiff_t>(Byte(Next + 2)) - 8;
            }
        }
        Natives::UNetDriver_DestroyedStartupOrDormantActorsIsUnique = DestroyedActorValueSize == static_cast<ptrdiff_t>(sizeof(void*));
        if (Natives::UNetDriver_DestroyedStartupOrDormantActorsIsUnique)
        {
            Log::Info(L"Finder: UNetDriver::DestroyedStartupOrDormantActors holds TUniquePtr<FActorDestructionInfo>");
        }
        else if (DestroyedActorValueSize && DestroyedActorValueSize != static_cast<ptrdiff_t>(Natives::FActorDestructionInfo_Size))
        {
            Log::Warning(L"Finder: UNetDriver::DestroyedStartupOrDormantActors holds 0x{:X} byte values, FActorDestructionInfo is 0x{:X}", DestroyedActorValueSize, Natives::FActorDestructionInfo_Size);
        }

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
        // 5.0 (UE 4.21) loads the frame early and checks it against the changelist state's last frame before it calls the
        // update: mov r14d, [rcx+370h]; ...; cmp [rbx+20h], r14d; ...; mov [rbx+20h], r14d (CL-4214610 0x2352302).
        for (uintptr_t Load = ReplicationFrameReads == 0 && ReplicatePropertiesEnd ? FindInstruction(ReplicateProperties, ReplicatePropertiesEnd, { 0x44, 0x8B }) : 0; Load; Load = FindInstruction(Load + 1, ReplicatePropertiesEnd, { 0x44, 0x8B }))
        {
            const uint8_t ModRm = Byte(Load + 2);
            if ((ModRm & 0xC0) != 0x80 || (ModRm & 0x07) == 0x04)
            {
                continue;
            }
            const int32_t Register = ModRm & 0x38;
            const auto UsesFrame = [&](uint8_t Opcode)
            {
                for (uintptr_t At = FindInstruction(Load + 7, ReplicatePropertiesEnd, { 0x44, Opcode }); At; At = FindInstruction(At + 1, ReplicatePropertiesEnd, { 0x44, Opcode }))
                {
                    if ((Byte(At + 2) & 0xC0) == 0x40 && (Byte(At + 2) & 0x38) == Register && (Byte(At + 2) & 0x07) != 0x04)
                    {
                        return true;
                    }
                }
                return false;
            };
            const ptrdiff_t Offset = *reinterpret_cast<const int32_t*>(Load + 3);
            if (UsesFrame(0x39) && UsesFrame(0x89) && Offset != ReplicationFrame)
            {
                ReplicationFrame = Offset;
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
        // Without the prediction (1.9) the viewer has no velocity call to compare: take the GetVelocity exec's alone.
        RequireSlot(L"AActor::GetVelocity", Natives::AActor_GetVelocityIndex, Velocity == ViewerVelocity || (!bNetViewerPredicts && TickCountTests == 0) ? Velocity : -1);
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
            // On 1.9.1 ProcessRemoteFunction, the caller, is moved code in pieces: read it in the order it runs.
            const uintptr_t Call = Finder::HasMovedCode() ? Finder::TraceFind(Site + 5, 16, [](uintptr_t Instruction) { return FindVirtualCall(Instruction, Instruction + 6) == Instruction; }) : FindVirtualCall(Site + 5, Site + 0x40);
            const int32_t Slot = VirtualSlot(Call);
            if (Slot < 0 || (NetRelevantSites && Slot != NetRelevant))
            {
                NetRelevantSites = 0;
                break;
            }
            NetRelevant = Slot;
            ++NetRelevantSites;
        }
        if (!NetRelevantSites)
        {
            // The FNetViewer call and the virtual call after it can be ciphertext when Core resolves (1.9.1): derive
            // the slot from the Actor vtable family in .rdata instead, the way ResolveReplicationLive compares it.
            const int32_t Slot = NetRelevantSlotFromVTables(FunctionOf(L"Actor %s / %s has no root component in AActor::IsNetRelevantFor. (Make bAlwaysRelevant=true?)"));
            if (Slot >= 0)
            {
                NetRelevant = Slot;
                NetRelevantSites = 1;
            }
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
            const uintptr_t DormancyTest = Function && Site - Function <= 0xB0 ? FindInstruction(Function, Site, { 0x80, -1, static_cast<int32_t>(NetDormancy & 0xFF), static_cast<int32_t>((NetDormancy >> 8) & 0xFF), 0x00, 0x00, 0x01 }) : 0;
            if (!DormancyTest || (Byte(DormancyTest + 1) & 0xF8) != 0xB8)
            {
                continue;
            }
            const auto IsTimeRead = [](const uintptr_t Load)
            {
                return Load && (Byte(Load + 3) & 0xC0) == 0x80 && (Byte(Load + 3) & 0x07) != 0x04 && FindInstruction(Load + 8, Load + 0x18, { 0xF3, 0x0F, 0x5C });
            };
            uintptr_t Load = FindInstruction(Site, Site + 0x30, { 0xF3, 0x0F, 0x10 });
            // 3.3's ForceNetUpdate (0x1E696B0) leaves the time to UNetDriver::ForceNetUpdate (0x21EC350), called just
            // before the dormancy test: NetActor->NextUpdateTime = World->TimeSeconds - 0.01f.
            uintptr_t DriverForceUpdate = 0;
            for (uintptr_t Call = IsTimeRead(Load) ? 0 : FindRawCall(Function, DormancyTest); Call; Call = FindRawCall(Call + 5, DormancyTest))
            {
                DriverForceUpdate = Finder::RelativeTarget(Call, 1, 5);
            }
            Load = DriverForceUpdate ? FindInstruction(DriverForceUpdate, DriverForceUpdate + 0xA0, { 0xF3, 0x0F, 0x10 }) : Load;
            // 3.5.1's UNetDriver::ForceNetUpdate (0x228F370) is virtual: AActor::ForceNetUpdate (0x1EFF0A0, its call to
            // FlushNetDormancy at +0xA3) calls it through the net driver's vtable, call [r8+2E8h].
            for (uintptr_t Call = IsTimeRead(Load) ? 0 : FindVirtualCall(Function, DormancyTest); Call && !IsTimeRead(Load); Call = FindVirtualCall(Call + 1, DormancyTest))
            {
                const uintptr_t Entry = VTableEntry(NetDriverVTable, VirtualSlot(Call));
                Load = Entry ? FindInstruction(Entry, Entry + 0xA0, { 0xF3, 0x0F, 0x10 }) : 0;
            }
            if (IsTimeRead(Load))
            {
                // Every read must agree.
                TimeSecondsReads = !TimeSecondsReads || TimeSeconds == *reinterpret_cast<const int32_t*>(Load + 4) ? TimeSecondsReads + 1 : -1000;
                TimeSeconds = *reinterpret_cast<const int32_t*>(Load + 4);
            }
        }
        // 5.0 (UE 4.21) inlines FlushNetDormancy into AActor::ForceNetUpdate (CL-4214610 0x217F380), so nothing above
        // calls it: take the functions that test NetDormancy > DORM_Awake (cmp byte ptr [reg+NetDormancy], 1) within
        // 0xB0 of their start, and the net driver virtual each calls before it (UNetDriver::ForceNetUpdate, slot 0x5F,
        // 0x24E1670: NextUpdateTime = World->TimeSeconds - 0.01f, movss xmm0, [rax+7A8h]; subss), read inside that
        // virtual's own body.
        for (uintptr_t Test = TimeSecondsReads == 0 && NetDormancy && NetDriverVTable ? FindInstruction(Finder::TextBegin(), Finder::TextEnd(), { 0x80, -1, static_cast<int32_t>(NetDormancy & 0xFF), static_cast<int32_t>((NetDormancy >> 8) & 0xFF), 0x00, 0x00, 0x01 }) : 0; Test;
             Test = FindInstruction(Test + 1, Finder::TextEnd(), { 0x80, -1, static_cast<int32_t>(NetDormancy & 0xFF), static_cast<int32_t>((NetDormancy >> 8) & 0xFF), 0x00, 0x00, 0x01 }))
        {
            const uintptr_t Function = (Byte(Test + 1) & 0xF8) == 0xB8 ? Finder::FunctionStart(Test) : 0;
            for (uintptr_t Call = Function && Test - Function <= 0xB0 ? FindVirtualCall(Function, Test) : 0; Call; Call = FindVirtualCall(Call + 1, Test))
            {
                const uintptr_t Entry = VTableEntry(NetDriverVTable, VirtualSlot(Call));
                const uintptr_t EntryEnd = Entry ? Finder::FunctionEnd(Entry) : 0;
                const uintptr_t Load = EntryEnd > Entry ? FindInstruction(Entry, (std::min)(EntryEnd, Entry + 0xA0), { 0xF3, 0x0F, 0x10 }) : 0;
                if (Load && Load + 0x18 <= EntryEnd && (Byte(Load + 3) & 0xC0) == 0x80 && (Byte(Load + 3) & 0x07) != 0x04 && FindInstruction(Load + 8, Load + 0x18, { 0xF3, 0x0F, 0x5C }))
                {
                    TimeSecondsReads = !TimeSecondsReads || TimeSeconds == *reinterpret_cast<const int32_t*>(Load + 4) ? TimeSecondsReads + 1 : -1000;
                    TimeSeconds = *reinterpret_cast<const int32_t*>(Load + 4);
                }
            }
        }
        RequireOffset(L"UWorld::TimeSeconds", Natives::UWorld_TimeSeconds, TimeSecondsReads >= 1 ? TimeSeconds : 0);

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
            // Both tests are single bits; 3.3 has another pair of tests a byte apart (0xE0E311: [reg+80h], 18h).
            const bool bSingleBits = Byte(Test + 6) && !(Byte(Test + 6) & (Byte(Test + 6) - 1)) && Byte(Test + 15) && !(Byte(Test + 15) & (Byte(Test + 15) - 1));
            if ((Byte(Test + 1) & 0xF8) == 0x80 && (Byte(Test + 1) & 0x07) != 0x04 && Byte(Test + 10) == Byte(Test + 1) && Byte(Test + 16) == 0x75 && bSingleBits
                && *reinterpret_cast<const int32_t*>(Test + 11) == *reinterpret_cast<const int32_t*>(Test + 2) + 1)
            {
                // 4.2 has the actor iterator's test in two functions (0x6D084A, 0x2493AD6): every copy must agree.
                const ptrdiff_t Offset = *reinterpret_cast<const int32_t*>(Test + 11);
                const bool bAgrees = !AssociatingLevelTests || (Offset == AssociatingLevel && Byte(Test + 15) == AssociatingLevelMask);
                AssociatingLevel = Offset;
                AssociatingLevelMask = Byte(Test + 15);
                AssociatingLevelTests = bAgrees ? AssociatingLevelTests + 1 : -1000;
            }
        }
        RequireOffset(L"ULevel::bIsAssociatingLevel", Natives::ULevel_bIsAssociatingLevel, AssociatingLevelTests >= 1 ? AssociatingLevel : 0);
        Natives::ULevel_bIsAssociatingLevelMask = Natives::ULevel_bIsAssociatingLevel > 0 ? AssociatingLevelMask : 0;

        const uintptr_t AdaptiveRef = OnlyRef(L"net.UseAdaptiveNetUpdateFrequency");
        uintptr_t AsVariableInt = AdaptiveRef ? FindInstruction(AdaptiveRef, AdaptiveRef + 0x40, { 0xFF, 0x52, 0x30 }) : 0;
        if (AdaptiveRef && !AsVariableInt)
        {
            // 1.10's IConsoleVariable has AsVariableInt one slot later (call [rdx+38h] at 0x1A46D9), its result stored next.
            AsVariableInt = FindInstruction(AdaptiveRef, AdaptiveRef + 0x40, { 0xFF, 0x52, -1, 0x48, 0x8D, 0x0D, -1, -1, -1, -1, 0x48, 0x89, 0x05 });
        }
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
        // The free index loop runs to MAX_CHANNELS: 0x2800 up to 2.1, 0x7FFF on 2.2.
        const bool bChannelLoop = CreateChannelEnd && (FindInstruction(CreateChannel, CreateChannelEnd, { 0x48, 0x3D, 0x00, 0x28, 0x00, 0x00 }) || FindInstruction(CreateChannel, CreateChannelEnd, { 0x48, 0x3D, 0xFF, 0x7F, 0x00, 0x00 }));
        ptrdiff_t Channels = 0;
        for (uintptr_t Store = bChannelLoop ? FindInstruction(CreateChannel, CreateChannelEnd, { 0x48, 0x89 }) : 0; Store && !Channels; Store = FindInstruction(Store + 1, CreateChannelEnd, { 0x48, 0x89 }))
        {
            if ((Byte(Store + 2) & 0xC7) == 0x84 && (Byte(Store + 3) >> 6) == 3)
            {
                Channels = *reinterpret_cast<const int32_t*>(Store + 4);
            }
        }
        // 2.4.2's Channels is a TArray<UChannel*>: the loop bound is its Num, movsxd r9, [rsi+0x1550], and the slots its
        // Data, mov rax, [rsi+0x1548] (CreateChannel 0x1B0C510). Take the Data load whose next qword is read as Num.
        Natives::UNetConnection_ChannelsIsArray = false;
        for (uintptr_t Load = !bChannelLoop && CreateChannelEnd && CreateChannel == NoFreeChannel ? FindInstruction(CreateChannel, CreateChannelEnd, { -1, 0x8B }) : 0; Load && !Channels; Load = FindInstruction(Load + 1, CreateChannelEnd, { -1, 0x8B }))
        {
            if ((Byte(Load) & 0xF8) != 0x48 || (Byte(Load + 2) & 0xC0) != 0x80 || (Byte(Load + 2) & 0x07) == 0x04)
            {
                continue;
            }
            const int32_t Data = *reinterpret_cast<const int32_t*>(Load + 3);
            const int32_t Num = Data + 8;
            for (uintptr_t Count = FindInstruction(CreateChannel, CreateChannelEnd, { -1, 0x63 }); Count && !Channels; Count = FindInstruction(Count + 1, CreateChannelEnd, { -1, 0x63 }))
            {
                if ((Byte(Count) & 0xF8) == 0x48 && (Byte(Count + 2) & 0xC0) == 0x80 && (Byte(Count + 2) & 0x07) == (Byte(Load + 2) & 0x07)
                    && (Byte(Count) & 0x01) == (Byte(Load) & 0x01) && *reinterpret_cast<const int32_t*>(Count + 3) == Num)
                {
                    Channels = Data;
                    Natives::UNetConnection_ChannelsIsArray = true;
                }
            }
        }
        RequireOffset(L"UNetConnection::Channels", Natives::UNetConnection_Channels, Channels);
        if (NamedCreate && CreateChannel)
        {
            Log::Info(L"Finder: channels are made by name (UE 4.22's): UNetConnection::CreateChannelByName +0x{:X}, Channels +0x{:X}, UNetDriver::IsLevelInitializedForActor slot 0x{:X}", CreateChannel - Finder::ImageBase(), Channels, LevelInitialized);
        }

        ptrdiff_t IsPeer = 0;
        size_t IsPeerSites = 0;
        for (const uintptr_t Site : Finder::StringRefs(L"(PEER)"))
        {
            const bool bPeerTest = Byte(Site - 7) == 0x80 && (Byte(Site - 6) & 0xF8) == 0xB8 && (Byte(Site - 6) & 0x07) != 0x04 && Byte(Site - 1) == 0x00;
            // 5.0 (UE 4.21) also compares with a zeroed register: cmp [r14+188h], r13b (45 38 AE; CL-4214610 0x24DE58F).
            const bool bPeerRegisterTest = !bPeerTest && (Byte(Site - 7) & 0xF0) == 0x40 && Byte(Site - 6) == 0x38 && (Byte(Site - 5) & 0xC0) == 0x80 && (Byte(Site - 5) & 0x07) != 0x04;
            const ptrdiff_t Offset = bPeerTest ? *reinterpret_cast<const int32_t*>(Site - 5) : bPeerRegisterTest ? *reinterpret_cast<const int32_t*>(Site - 4) : 0;
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
            // mov r9d, [rax+Bits] up to 2.5, mov r8d on 3.0: any of r8d-r15d read from the channel just loaded.
            const std::initializer_list<int32_t> ReadBits = { 0x44, 0x8B, -1, static_cast<int32_t>(ActorChannelBits & 0xFF), static_cast<int32_t>((ActorChannelBits >> 8) & 0xFF), 0x00, 0x00 };
            uintptr_t BitsRead = FindInstruction(Load + 7, Load + 0x30, ReadBits);
            while (BitsRead && (Byte(BitsRead + 2) & 0xC7) != 0x80)
            {
                BitsRead = FindInstruction(BitsRead + 1, Load + 0x30, ReadBits);
            }
            if (BitsRead)
            {
                OwningChannel = *reinterpret_cast<const int32_t*>(Load + 3);
                ++OwningChannelLoads;
            }
        }
        // 5.0 (UE 4.21) reads the bits into ecx: mov rax, [rdi+0F8h]; ...; mov ecx, [rax+88h] (CL-4214610 0x23522D0).
        // Only a load into rax counts here, the register the bits are read through.
        for (uintptr_t Load = OwningChannelLoads == 0 && ReplicatePropertiesEnd && ActorChannelBits > 0 ? FindInstruction(ReplicateProperties, ReplicatePropertiesEnd, { 0x48, 0x8B }) : 0; Load; Load = FindInstruction(Load + 1, ReplicatePropertiesEnd, { 0x48, 0x8B }))
        {
            if ((Byte(Load + 2) & 0xF8) != 0x80 || (Byte(Load + 2) & 0x07) == 0x04)
            {
                continue;
            }
            const std::initializer_list<int32_t> ReadBits = { 0x8B, -1, static_cast<int32_t>(ActorChannelBits & 0xFF), static_cast<int32_t>((ActorChannelBits >> 8) & 0xFF), 0x00, 0x00 };
            uintptr_t BitsRead = FindInstruction(Load + 7, Load + 0x30, ReadBits);
            while (BitsRead && ((Byte(BitsRead + 1) & 0xC7) != 0x80 || (Byte(BitsRead - 1) & 0xF0) == 0x40))
            {
                BitsRead = FindInstruction(BitsRead + 1, Load + 0x30, ReadBits);
            }
            if (BitsRead)
            {
                OwningChannel = *reinterpret_cast<const int32_t*>(Load + 3);
                ++OwningChannelLoads;
            }
        }
        // 5.40 (CL-4352937) loads the channel into rcx and reads the bits through it into eax: mov rcx, [rbx+0F8h];
        // mov eax, [rcx+88h] (0x2491235). The bits read's base is the register the load wrote (no REX on either side).
        for (uintptr_t Load = OwningChannelLoads == 0 && ReplicatePropertiesEnd && ActorChannelBits > 0 ? FindInstruction(ReplicateProperties, ReplicatePropertiesEnd, { 0x48, 0x8B }) : 0; Load; Load = FindInstruction(Load + 1, ReplicatePropertiesEnd, { 0x48, 0x8B }))
        {
            if ((Byte(Load + 2) & 0xC0) != 0x80 || (Byte(Load + 2) & 0x07) == 0x04)
            {
                continue;
            }
            const uint8_t Channel = (Byte(Load + 2) >> 3) & 0x07;
            const std::initializer_list<int32_t> ReadBits = { 0x8B, -1, static_cast<int32_t>(ActorChannelBits & 0xFF), static_cast<int32_t>((ActorChannelBits >> 8) & 0xFF), 0x00, 0x00 };
            uintptr_t BitsRead = FindInstruction(Load + 7, Load + 0x30, ReadBits);
            while (BitsRead && ((Byte(BitsRead + 1) & 0xC7) != (0x80 | Channel) || Channel == 0x04 || (Byte(BitsRead - 1) & 0xF0) == 0x40))
            {
                BitsRead = FindInstruction(BitsRead + 1, Load + 0x30, ReadBits);
            }
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
        // 3.5 keeps UNetDriver::FlushActorDormancy (3.5.1 0x228E710): FlushNetDormancy and SetNetDormancy call it for the
        // net driver and the demo net driver, and Core leaves those calls alone.
        bool bFlushDormancyIntact = AddNetworkActor != 0;
        uintptr_t IntactFlushDormancy = 0;
        for (const uintptr_t Function : { FlushDormancy, SetDormancyFunctions == 1 ? SetDormancy : uintptr_t(0) })
        {
            const uintptr_t End = Finder::FunctionEnd(Function);
            uintptr_t AddActorCall = 0;
            for (uintptr_t Call = End ? FindRawCall(Function, End) : 0; Call && !AddActorCall; Call = FindRawCall(Call + 5, End))
            {
                AddActorCall = Finder::RelativeTarget(Call, 1, 5) == AddNetworkActor ? Call : 0;
            }

            int32_t Calls = 0;
            std::unordered_map<uintptr_t, int32_t> TargetCalls;
            for (uintptr_t Call = AddActorCall ? FindRawCall(AddActorCall + 5, End) : 0; Call; Call = FindRawCall(Call + 5, End))
            {
                const uintptr_t Target = Finder::RelativeTarget(Call, 1, 5);
                ++TargetCalls[Target];
                if (IsSharedEmptyBody(Target) && (!StrippedFlushDormancy || Target == StrippedFlushDormancy))
                {
                    StrippedFlushDormancy = Target;
                    Sites::FlushActorDormancyCalls.push_back(reinterpret_cast<uint8_t*>(Call));
                    ++Calls;
                }
            }
            bFlushDormancyCalls = bFlushDormancyCalls && Calls == 2;

            uintptr_t Twice = 0;
            for (const auto& [Target, Count] : TargetCalls)
            {
                Twice = Count == 2 && !IsSharedEmptyBody(Target) && (!IntactFlushDormancy || Target == IntactFlushDormancy) ? Target : Twice;
            }
            IntactFlushDormancy = Twice ? Twice : IntactFlushDormancy;
            bFlushDormancyIntact = bFlushDormancyIntact && Calls == 0 && Twice;
        }
        if (!bFlushDormancyCalls && bFlushDormancyIntact)
        {
            Log::Info(L"Finder: UNetDriver::FlushActorDormancy is intact at +0x{:X}, its calls stay", IntactFlushDormancy - Finder::ImageBase());
            Sites::FlushActorDormancyCalls.clear();
        }
        else if (!bFlushDormancyCalls)
        {
            Log::Error(L"Finder: UNetDriver::FlushActorDormancy call sites not found ({})", Sites::FlushActorDormancyCalls.size());
            Sites::FlushActorDormancyCalls.clear();
            ++MissingCount;
        }
    }
}

namespace
{
    // bForceDedicated's offset in FOnlineSubsystemImpl::IsDedicated: xor eax, eax; cmp [rcx+X], al up to 4.5, or (UE
    // 4.21, 5.0) cmp byte ptr [rcx+X], 0.
    int32_t OnlineSubsystemForceDedicatedOffset(uintptr_t IsDedicated)
    {
        return *reinterpret_cast<const int32_t*>(IsDedicated + (Byte(IsDedicated) == 0x80 ? 2 : 4));
    }

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
        // On 1.9.1 (Finder::HasMovedCode) junk replaced the int3 padding between functions.
        return Start == Address || (!Start && (Address & 0xF) == 0 && (Byte(Address - 1) == 0xCC || Byte(Address - 1) == 0xC3 || Finder::HasMovedCode()));
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
            const bool bReturnsNull = Finder::BodyIs(Target, { 0x33, 0xC0, 0xC3 });
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
        uintptr_t ZoneSpawnPawn = UniqueFunction({ 0x4D, 0x85, 0xC0, 0x74, 0x47, 0x48, 0x85, 0xD2, 0x74, 0x42, 0xE8 });
        if (!ZoneSpawnPawn)
        {
            // 5.0 (UE 4.21) has a longer body behind the same two null checks: test r8, r8; jz +51h; test rdx, rdx; jz
            // +4Ch; call (CL-4214610 0xE10EF0). Both jumps still meet; the one such pair.
            int32_t Pairs = 0;
            const std::initializer_list<int32_t> NullChecks = { 0x4D, 0x85, 0xC0, 0x74, -1, 0x48, 0x85, 0xD2, 0x74, -1, 0xE8 };
            for (uintptr_t Site = FindInstruction(Finder::TextBegin(), Finder::TextEnd(), NullChecks); Site; Site = FindInstruction(Site + 1, Finder::TextEnd(), NullChecks))
            {
                if (Site + 5 + Byte(Site + 4) == Site + 10 + Byte(Site + 9) && Finder::FunctionStart(Site))
                {
                    ZoneSpawnPawn = Site;
                    ++Pairs;
                }
            }
            ZoneSpawnPawn = Pairs == 1 ? ZoneSpawnPawn : 0;
        }
        Require(L"AFortGameModeZone::SpawnDefaultPawnFor_Implementation", Natives::AFortGameModeZone_SpawnDefaultPawnFor, ZoneSpawnPawn ? Finder::FunctionStart(ZoneSpawnPawn) : 0);
        Require(L"AFortGameMode::SpawnDefaultPawnFor_Implementation", Natives::AFortGameMode_SpawnDefaultPawnFor, FunctionOf(L"Couldn't spawn player pawn of type %s due to no start spot"));

        // or dword ptr [rdi+bWorldIsReady], 1; call [rax+GetWorld]; mov rcx, rax (bWorldIsReady +0x530 on 1.1 and 1.2,
        // +0x568 on 1.7.2; the plain or has dozens of matches, the call after it makes it unique).
        const uintptr_t WorldIsReady = UniqueFunction({ 0x83, 0x8F, -1, -1, 0x00, 0x00, 0x01, 0xFF, 0x90, -1, -1, 0x00, 0x00, 0x48, 0x8B, 0xC8 });
        Require(L"AFortGameMode::FinishWorldInitialization", Natives::AFortGameMode_FinishWorldInitialization, WorldIsReady ? Finder::FunctionStart(WorldIsReady) : 0);

        Require(L"AFortWorldManager::OnWorldRecordQueryComplete", Natives::AFortWorldManager_OnWorldRecordQueryComplete, FunctionOf(L"Invalid world record when transition to world query complete state!"));
        Require(L"AFortWorldManager::ServerTickLoadingOrCreatingWorld", Natives::AFortWorldManager_ServerTickLoadingOrCreatingWorld, FunctionOf(L"Skipping loading of level %s, LevelStreaming does not exist"));

        Sites::AIDirectorGetCurrentCalls.clear();
        size_t ExpectedDirectorCalls = 0;
        for (const wchar_t* Text : { L"StartMissionAIEncounter: No AI Director!", L"StartMissionAIEncounterFromGeneratedProfile: No AI Director!", L"Failed to set simulated number of players, no AI Director found.", L"Failed to clear simulated number of players, no AI Director found." })
        {
            // 4.2 drops the two simulated number of players cheats, lines and all; the mission encounter ones stay.
            const bool bSimulatedPlayers = std::wcsstr(Text, L"simulated number of players") != nullptr;
            if (bSimulatedPlayers && Finder::StringRefs(Text).empty())
            {
                continue;
            }
            ++ExpectedDirectorCalls;
            const uintptr_t Function = FunctionOf(Text);
            const uintptr_t FunctionEnd = Function ? Finder::FunctionEnd(Function) : 0;
            uintptr_t GetCurrentCall = 0;
            for (uintptr_t Call = FunctionEnd ? FindRawCall(Function, FunctionEnd) : 0; Call && !GetCurrentCall; Call = FindRawCall(Call + 1, FunctionEnd))
            {
                const uintptr_t Callee = Finder::RelativeTarget(Call, 1, 5);
                if (Finder::BodyIs(Callee, { 0x33, 0xC0, 0xC3 }))
                {
                    GetCurrentCall = Call;
                }
            }
            if (GetCurrentCall)
            {
                Sites::AIDirectorGetCurrentCalls.push_back(reinterpret_cast<uint8_t*>(GetCurrentCall));
            }
        }
        if (ExpectedDirectorCalls < 2 || Sites::AIDirectorGetCurrentCalls.size() != ExpectedDirectorCalls)
        {
            ++MissingCount;
            Log::Error(L"Finder: AFortAIDirector::GetCurrent call sites: found {} of {}", Sites::AIDirectorGetCurrentCalls.size(), ExpectedDirectorCalls);
        }

        // 3.3 (0xB9D00C) drops the GameAccountId from the line.
        const uintptr_t ShortProfileFailed = First(Finder::StringRefs(L"Failed to initialize profile for player: UniqueId:[%s]"));
        const uintptr_t ProfileFailed = ShortProfileFailed ? ShortProfileFailed : OnlyRef(L"Failed to initialize profile for player: UniqueId:[%s] GameAccountId=[%s]");
        uintptr_t InitializeMcpProfile = 0;
        for (uintptr_t Call = ProfileFailed ? FindRawCall(ProfileFailed - 0x140, ProfileFailed) : 0; Call; Call = FindRawCall(Call + 1, ProfileFailed))
        {
            if (FindInstruction(Call + 5, Call + 0x10, { 0x84, 0xC0 }))
            {
                InitializeMcpProfile = Finder::RelativeTarget(Call, 1, 5);
            }
        }
        Require(L"AFortPlayerController::InitializeMcpProfile", Natives::AFortPlayerController_InitializeMcpProfile, InitializeMcpProfile);

        uintptr_t UniqueIdToString = ProfileFailed && !ShortProfileFailed ? FindBackward(ProfileFailed, 0x60, { 0xFF, 0x50, -1 }) : 0;
        // IsValid is the `call [rax+18h]; test al, al` just before it (slot 3, ToString 4, on 1.1, 1.2, 1.7.2 and 1.8);
        // AFortGameMode::ShouldPlayerEarnScore and the game session id ask it on the server.
        uintptr_t UniqueIdIsValid = UniqueIdToString ? FindBackward(UniqueIdToString, 0x20, { 0xFF, 0x50, -1, 0x84, 0xC0 }) : 0;
        // 3.3 logs FUniqueNetIdWrapper::ToDebugString (0x347590, 4.5 0x33AC70), the last call before the line: IsValid
        // (call [rax+20h]; test al, al), then "%s:%s" of the id's type and its ToDebugString (call [rax+30h]). The id's
        // ToString is the slot before ToDebugString (21.40's FUniqueNetId: IsValid, ToString, ToDebugString; 3.2's
        // ToString is the [rax+28h] the long line calls). Core read ToDebugString, which 4.0's prints whole but 4.4.2's and
        // 4.5's redact to the first and last five characters ("d8b43...352c0"): no account id, so the match results
        // skipped every player, the end-of-match screen got no stats and no rewards (all zeros, "Team Still Alive"), and
        // the warmup reported no players.
        // Only a call whose target asks IsValid counts: 5.30 (CL-4305896) has lea r8, [rbp-18h] (4C 8D 45 E8) after
        // the call (0xE2DCD1), an E8 byte the raw scan took for a later call into .text.
        uintptr_t WrapperToDebugString = 0;
        for (uintptr_t Call = ShortProfileFailed ? FindRawCall(ShortProfileFailed - 0x30, ShortProfileFailed) : 0; Call; Call = FindRawCall(Call + 5, ShortProfileFailed))
        {
            const uintptr_t Target = Finder::RelativeTarget(Call, 1, 5);
            if (FindInstruction(Target, Target + 0x40, { 0xFF, 0x50, -1, 0x84, 0xC0 }))
            {
                WrapperToDebugString = Target;
            }
        }
        int32_t UniqueIdToDebugStringIndex = -1;
        if (WrapperToDebugString)
        {
            UniqueIdIsValid = FindInstruction(WrapperToDebugString, WrapperToDebugString + 0x40, { 0xFF, 0x50, -1, 0x84, 0xC0 });
            const uintptr_t UniqueIdToDebugString = UniqueIdIsValid ? FindInstruction(UniqueIdIsValid + 5, UniqueIdIsValid + 0x40, { 0xFF, 0x50, -1 }) : 0;
            UniqueIdToDebugStringIndex = UniqueIdToDebugString && Byte(UniqueIdToDebugString + 2) % 8 == 0 ? Byte(UniqueIdToDebugString + 2) / 8 : -1;
            UniqueIdToString = 0;
        }
        if (UniqueIdToDebugStringIndex >= 0)
        {
            Natives::FUniqueNetId_ToStringIndex = UniqueIdToDebugStringIndex - 1;
        }
        else
        {
            Natives::FUniqueNetId_ToStringIndex = UniqueIdToString && Byte(UniqueIdToString + 2) % 8 == 0 ? Byte(UniqueIdToString + 2) / 8 : -1;
        }
        if (Natives::FUniqueNetId_ToStringIndex < 0)
        {
            Log::Error(L"Finder: FUniqueNetId::ToString slot not found");
        }
        const int32_t UniqueIdIsValidIndex = UniqueIdIsValid && Byte(UniqueIdIsValid + 2) % 8 == 0 ? Byte(UniqueIdIsValid + 2) / 8 : -1;
        Natives::FUniqueNetId_IsValidIndex = UniqueIdIsValidIndex >= 0 && UniqueIdIsValidIndex < Natives::FUniqueNetId_ToStringIndex ? UniqueIdIsValidIndex : -1;
        if (Natives::FUniqueNetId_IsValidIndex < 0)
        {
            Log::Error(L"Finder: FUniqueNetId::IsValid slot not found");
        }

        Require(L"AFortPlayerState::SetHeroType", Natives::AFortPlayerState_SetHeroType, FunctionOf(L"Hero gender does not match gender of HeroType %s!  Hero Gender is %d instead."));
        // Dresses the player state from its controller's hero, and from 2.4.2 the athena profile's favorite back bling
        // (1.9 0x87A800 to 3.0 0xE75BB0). It logs this string when the parts chooser fails; 1.1 to 1.8.2 don't have it.
        Natives::AFortPlayerState_ApplyCharacterCustomization = reinterpret_cast<decltype(Natives::AFortPlayerState_ApplyCharacterCustomization)>(
            FunctionOf(L"AFortPlayerState::ApplyCharacterCustomization - Failed initialization, using default parts. Player Controller: %s PlayerState: %s, HeroId: %s"));
        if (Natives::AFortPlayerState_ApplyCharacterCustomization)
        {
            Log::Info(L"Finder: AFortPlayerState::ApplyCharacterCustomization at +0x{:X}", reinterpret_cast<uintptr_t>(Natives::AFortPlayerState_ApplyCharacterCustomization) - Finder::ImageBase());
        }
        else
        {
            Log::Info(L"Finder: AFortPlayerState::ApplyCharacterCustomization isn't in this build (before 1.9); Core dresses the player state itself");
        }
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
        uintptr_t TrySetHero = 0;
        if (SetHero && !Finder::ReadOnlySlotsHolding(SetHero).empty())
        {
            // 2.4.2 made the player state's hero setter virtual (slot 0x700, 0x8F2380), and Battle Royale's player state
            // overrides it (0x478900: its own, then the Super call). Its only direct caller is that override, which Core
            // took for TrySetHeroImplementation and detoured: Possess's SetupHero then ran the player state's setter on
            // the controller and crashed on its loading screen (#208). TrySetHeroImplementation (0x8AF260) calls the
            // setter through the vtable and, right after, the player state's next function (0x8F2540, which follows
            // the setter in the image as on 2.4: 0x8E4230, then 0x8E43F0). Its other caller passes another call's result.
            const uintptr_t SetHeroEnd = Finder::FunctionEnd(SetHero);
            uintptr_t AfterSetHero = 0;
            for (uintptr_t At = SetHeroEnd; At && At < SetHeroEnd + 0x40 && !AfterSetHero; ++At)
            {
                if (Byte(At) != 0xCC && Finder::FunctionStart(At) == At)
                {
                    AfterSetHero = At;
                }
            }
            for (uintptr_t Call : AfterSetHero ? Finder::CallsTo(AfterSetHero, false) : std::vector<uintptr_t>{})
            {
                bool bAfterVirtualSet = false;
                for (uintptr_t Back = 6; Back <= 0x10 && !bAfterVirtualSet; ++Back)
                {
                    const uintptr_t At = Call - Back;
                    const uint32_t Offset = *reinterpret_cast<const uint32_t*>(At + 2);
                    bAfterVirtualSet = Byte(At) == 0xFF && (Byte(At + 1) & 0xF8) == 0x90 && Byte(At + 1) != 0x94 && Offset % 8 == 0 && Offset < 0x4000;
                }
                const uintptr_t Caller = bAfterVirtualSet ? Finder::FunctionStart(Call) : 0;
                if (Caller && TrySetHero && Caller != TrySetHero)
                {
                    TrySetHero = 0;
                    break;
                }
                TrySetHero = Caller ? Caller : TrySetHero;
            }
        }
        else
        {
            TrySetHero = OnlyCaller(SetHero);
        }
        Require(L"AFortPlayerController::TrySetHeroImplementation", Natives::AFortPlayerController_TrySetHeroImplementation, TrySetHero);

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
        // 1.10 (0x5ED969) holds the interface in r12 instead of r14: lea r12, [rsi+170h]; ...; mov rcx, r12; call [rax+20h].
        const std::initializer_list<int32_t> AnyHasAllCall = { 0x49, 0x8B, -1, 0xFF, 0x50 };
        for (uintptr_t Call = !TagAssetInterface && ApplyHeroEnd ? FindInstruction(ApplyHero, ApplyHeroEnd, AnyHasAllCall) : 0; Call && !TagAssetInterface; Call = FindInstruction(Call + 1, ApplyHeroEnd, AnyHasAllCall))
        {
            const uint8_t Register = Byte(Call + 2) & 0x07;
            if ((Byte(Call + 2) & 0xF8) == 0xC8)
            {
                TagAssetInterface = FindBackward(Call, 0x30, { 0x4C, 0x8D, 0x86 | Register << 3 });
                // 2.2 (0x5F8659) bases it on r15: lea r13, [r15+170h].
                TagAssetInterface = TagAssetInterface ? TagAssetInterface : FindBackward(Call, 0x30, { 0x4D, 0x8D, 0x87 | Register << 3 });
                HasAll = Call + 3;
            }
        }
        // 5.0 (UE 4.21) loads the interface's vtable from the component and passes the interface itself straight to the
        // call: mov rax, [r15+170h]; lea rdx, [rdi+28h]; lea rcx, [r15+170h]; call [rax+20h] (CL-4214610 0xE32020).
        for (uintptr_t Lea = !TagAssetInterface && ApplyHeroEnd ? FindInstruction(ApplyHero, ApplyHeroEnd, { -1, 0x8D, -1, -1, -1, 0x00, 0x00, 0xFF, 0x50 }) : 0; Lea && !TagAssetInterface; Lea = FindInstruction(Lea + 1, ApplyHeroEnd, { -1, 0x8D, -1, -1, -1, 0x00, 0x00, 0xFF, 0x50 }))
        {
            const uint8_t Rex = Byte(Lea);
            const uint8_t ModRm = Byte(Lea + 2);
            if ((Rex & 0xFC) != 0x48 || (ModRm & 0xF8) != 0x88 || (ModRm & 0x07) == 0x04)
            {
                continue;
            }
            const int32_t Offset = *reinterpret_cast<const int32_t*>(Lea + 3);
            // mov rax, [base+Offset] shortly before, from the same base.
            const uint8_t VTableLoad[] = { Rex, 0x8B, static_cast<uint8_t>(0x80 | (ModRm & 0x07)) };
            for (uintptr_t Load = Lea > 0x18 ? Lea - 0x18 : 0; Load && Load + 7 <= Lea && !TagAssetInterface; ++Load)
            {
                if (memcmp(reinterpret_cast<const void*>(Load), VTableLoad, sizeof(VTableLoad)) == 0 && *reinterpret_cast<const int32_t*>(Load + 3) == Offset)
                {
                    TagAssetInterface = Lea;
                    HasAll = Lea + 7;
                }
            }
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
        // 5.0 (CL-4214610 0xDEDE50) asks for the goal manager a fourth time (0xDEE45F), each the same stub call.
        if (GoalManagerCalls.size() == 3 || GoalManagerCalls.size() == 4)
        {
            Sites::ChoosePlayerStartGoalManagerCalls = std::move(GoalManagerCalls);
        }
        else
        {
            Log::Error(L"Finder: AFortGameModeZone::ChoosePlayerStart has {} AFortAIGoalManager::GetCurrent calls, expected 3 or 4", GoalManagerCalls.size());
            ++MissingCount;
        }

        uintptr_t RecordLoaded = UniqueFunction({ 0x83, 0xA1, 0xE0, 0x03, 0x00, 0x00, 0xFD, 0x0F, 0xB6, 0xC2, 0x83, 0xE0, 0x01, 0x03, 0xC0, 0x09, 0x81, 0xE0, 0x03, 0x00, 0x00 });
        if (!RecordLoaded)
        {
            // 1.10's beacon host keeps the bitfield at +0x3C8 (0x75C160). Other setters of bit 1 share the shape, but only
            // this one goes on to test the same bitfield.
            int32_t Matches = 0;
            const std::initializer_list<int32_t> SetBit = { 0x83, 0xA1, -1, -1, 0x00, 0x00, 0xFD, 0x0F, 0xB6, 0xC2, 0x83, 0xE0, 0x01, 0x03, 0xC0, 0x09, 0x81, -1, -1, 0x00, 0x00, 0xF6, 0x81 };
            for (uintptr_t Site = FindInstruction(Finder::TextBegin(), Finder::TextEnd(), SetBit); Site; Site = FindInstruction(Site + 1, Finder::TextEnd(), SetBit))
            {
                const int32_t Bitfield = *reinterpret_cast<const int32_t*>(Site + 2);
                if (*reinterpret_cast<const int32_t*>(Site + 17) == Bitfield && *reinterpret_cast<const int32_t*>(Site + 23) == Bitfield)
                {
                    RecordLoaded = Site;
                    ++Matches;
                }
            }
            RecordLoaded = Matches == 1 ? RecordLoaded : 0;
        }
        if (!RecordLoaded)
        {
            // 5.0 (UE 4.21) works on a copy in r8d (CL-4214610 0xFE02E0, no .pdata entry): mov eax, [rcx+388h]; and eax,
            // 0FFFFFFFDh; movzx r8d, dl; add r8d, r8d; or r8d, eax; mov [rcx+388h], r8d; test r8b, 2.
            int32_t Matches = 0;
            const std::initializer_list<int32_t> SetBit = { 0x8B, 0x81, -1, -1, 0x00, 0x00, 0x83, 0xE0, 0xFD, 0x44, 0x0F, 0xB6, 0xC2, 0x45, 0x03, 0xC0, 0x44, 0x0B, 0xC0, 0x44, 0x89, 0x81, -1, -1, 0x00, 0x00, 0x41, 0xF6, 0xC0, 0x02 };
            for (uintptr_t Site = FindInstruction(Finder::TextBegin(), Finder::TextEnd(), SetBit); Site; Site = FindInstruction(Site + 1, Finder::TextEnd(), SetBit))
            {
                if (*reinterpret_cast<const int32_t*>(Site + 2) == *reinterpret_cast<const int32_t*>(Site + 22))
                {
                    RecordLoaded = Site;
                    ++Matches;
                }
            }
            RecordLoaded = Matches == 1 ? RecordLoaded : 0;
        }
        Require(L"AFortLobbyBeaconHost::SetWorldRecordLoaded", Natives::AFortLobbyBeaconHost_SetWorldRecordLoaded, IsFunctionEntry(RecordLoaded) ? RecordLoaded : 0);

        const uintptr_t FlushDormancy = reinterpret_cast<uintptr_t>(Natives::AActor_FlushNetDormancy);
        uintptr_t SharedEmpty = 0;
        for (uintptr_t Call = FlushDormancy ? FindRawCall(FlushDormancy, Finder::FunctionEnd(FlushDormancy)) : 0; Call && !SharedEmpty; Call = FindRawCall(Call + 5, Finder::FunctionEnd(FlushDormancy)))
        {
            const uintptr_t Target = Finder::RelativeTarget(Call, 1, 5);
            SharedEmpty = IsSharedEmptyBody(Target) ? Target : 0;
        }
        if (!SharedEmpty && Finder::HasMovedCode() && Finder::BodyIs(Finder::MostReferencedFunction(), { 0xC2, 0x00, 0x00 }))
        {
            // 3.5's FlushNetDormancy calls the intact UNetDriver::FlushActorDormancy: the shared empty function is the one
            // most vtable slots hold (3.5.1 0xC0B7D0, 82,620 of them).
            SharedEmpty = Finder::MostReferencedFunction();
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
        // 3.5.1 (0xE2D906) reloads the config from the stack and adds the offset: mov rcx, [rbp+X]; add rcx, 98h; call.
        for (uintptr_t Add = !PermissionAdds && PermissionsEnd ? FindInstruction(Permissions, PermissionsEnd, { 0x48, 0x81, 0xC1 }) : 0; Add; Add = FindInstruction(Add + 1, PermissionsEnd, { 0x48, 0x81, 0xC1 }))
        {
            const uintptr_t AddCall = FindRawCall(Add + 7, Add + 0x10);
            const ptrdiff_t Offset = *reinterpret_cast<const int32_t*>(Add + 3);
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

        const uintptr_t HttpRequestCopy = Scan({ 0x48, 0x8B, 0x42, -1, 0x48, 0x89, 0x45, -1, 0x48, 0x8B, 0x42, -1 });
        const bool bHttpRequestCopy = HttpRequestCopy && Byte(HttpRequestCopy + 11) == Byte(HttpRequestCopy + 3) + 8;
        // 1.8.1 (0x3C53E30) and 1.8.2 (0x3C5D4C0) test the profile with mov rcx, [rdx]; test rcx, rcx; jz and hand
        // PrepareRequest the request object itself instead of a copy of its TSharedRef: mov rbx, [rdx+50h];
        // mov r8d, [rdx+64h]; mov rdx, rbx; ...; call [rax+270h] (their PrepareRequest uses rdx as the IHttpRequest).
        const uintptr_t ObjectPass = bHttpRequestCopy ? 0 : Scan({ 0x48, 0x8B, 0x0A, 0x48, 0x85, 0xC9, 0x74, -1, 0x48, 0x8B, -1, -1, 0x44, 0x8B, 0x42, -1, 0x48, 0x8B, -1 });
        const bool bObjectPass = ObjectPass && (Byte(ObjectPass + 10) & 0xC7) == 0x42 && Byte(ObjectPass + 18) == (0xD0 | ((Byte(ObjectPass + 10) >> 3) & 0x07));

        if (Scan({ 0x48, 0x83, 0x3A, 0x00 }) || bObjectPass)
        {
            Natives::FProfileHttpRequest_Profile = 0;
        }
        else
        {
            Log::Error(L"Finder: FProfileHttpRequest::Profile not found");
            ++MissingCount;
        }

        const uintptr_t HttpRequestLoad = bHttpRequestCopy ? HttpRequestCopy : (bObjectPass ? ObjectPass + 8 : 0);
        Natives::UMcpProfile_PrepareRequestTakesObject = !bHttpRequestCopy && bObjectPass;
        RequireOffset(L"FProfileHttpRequest::HttpRequest", Natives::FProfileHttpRequest_HttpRequest, HttpRequestLoad ? Byte(HttpRequestLoad + 3) : 0);

        const uintptr_t PrepareCall = HttpRequestLoad ? FindVirtualCall(HttpRequestLoad, HttpRequestLoad + 0x40) : 0;
        RequireSlot(L"UMcpProfile::PrepareRequest", Natives::UMcpProfile_PrepareRequestIndex, VirtualSlot(PrepareCall));
        const uintptr_t ResponseTypeLoad = PrepareCall ? FindInstruction(HttpRequestLoad, PrepareCall, { 0x44, 0x8B, 0x42, -1 }) : 0;
        RequireOffset(L"FProfileHttpRequest::ResponseType", Natives::FProfileHttpRequest_ResponseType, ResponseTypeLoad ? Byte(ResponseTypeLoad + 3) : 0);

        const uintptr_t ServiceCopy = Scan({ 0x49, 0x8B, 0x46, -1, -1, -1, -1, 0x48, 0x8B, 0xB0, -1, -1, -1, -1, 0x4C, 0x8B, 0xB8, -1, -1, -1, -1 });
        int32_t ServiceObject = ServiceCopy ? *reinterpret_cast<const int32_t*>(ServiceCopy + 17) : 0;
        bool bService = ServiceCopy && *reinterpret_cast<const int32_t*>(ServiceCopy + 10) == ServiceObject + 8;
        ptrdiff_t McpSubsystem = bService ? Byte(ServiceCopy + 3) : 0;
        if (!bService)
        {
            // 1.10 (0x2E314E0) keeps the profile group in rsi and copies the service through FOnlineSubsystemMcp's getter
            // (0x2A82120: mov rax, [rcx+378h] ... mov rax, [rcx+380h]): mov rcx, [rsi+110h]; lea rdx, [rbp+x]; ...; call.
            for (uintptr_t Load = Scan({ 0x48, 0x8B, 0x8E, -1, -1, -1, -1, 0x48, 0x8D, 0x55 }); Load && !bService; Load = FindInstruction(Load + 1, SendRequestNowEnd, { 0x48, 0x8B, 0x8E, -1, -1, -1, -1, 0x48, 0x8D, 0x55 }))
            {
                const uintptr_t GetterCall = FindRawCall(Load + 11, Load + 0x18);
                const uintptr_t Getter = GetterCall ? Finder::RelativeTarget(GetterCall, 1, 5) : 0;
                const uintptr_t Read = Getter ? FindInstruction(Getter, Getter + 0x10, { 0x48, 0x8B, 0x81 }) : 0;
                ServiceObject = Read ? *reinterpret_cast<const int32_t*>(Read + 3) : 0;
                uintptr_t ReadReference = Read ? FindInstruction(Read + 7, Read + 0x40, { 0x48, 0x8B, 0x81 }) : 0;
                // 5.0 (UE 4.21) reads the reference controller into rcx itself: mov rcx, [rcx+280h] (CL-4214610 0x5863C9).
                for (uintptr_t Candidate = Read && !(ReadReference && *reinterpret_cast<const int32_t*>(ReadReference + 3) == ServiceObject + 8) ? FindInstruction(Read + 7, Read + 0x40, { 0x48, 0x8B }) : 0; Candidate; Candidate = FindInstruction(Candidate + 1, Read + 0x40, { 0x48, 0x8B }))
                {
                    if ((Byte(Candidate + 2) & 0xC7) == 0x81 && *reinterpret_cast<const int32_t*>(Candidate + 3) == ServiceObject + 8)
                    {
                        ReadReference = Candidate;
                        break;
                    }
                }
                if (ReadReference && *reinterpret_cast<const int32_t*>(ReadReference + 3) == ServiceObject + 8)
                {
                    McpSubsystem = *reinterpret_cast<const int32_t*>(Load + 3);
                    bService = true;
                }
            }
        }
        RequireOffset(L"UMcpProfileGroup::McpSubsystem", Natives::UMcpProfileGroup_McpSubsystem, bService ? McpSubsystem : 0);
        RequireOffset(L"FOnlineSubsystemMcp::McpGameService", Natives::FOnlineSubsystemMcp_McpGameService, bService ? ServiceObject : 0);

        const uintptr_t ContextSwitch = Scan({ 0x8B, 0x4F, -1, 0x33, 0xDB, 0x48, 0x89, 0x5D, -1, 0x8B, 0xD3, 0x48, 0x89, 0x5D, -1, 0x85, 0xC9, 0x0F, 0x84 });
        RequireOffset(L"FProfileHttpRequest::ContextCredentials", Natives::FProfileHttpRequest_ContextCredentials, ContextSwitch ? Byte(ContextSwitch + 2) : 0);

        uintptr_t CredentialsCheck = Scan({ 0x49, 0x8B, 0x8E, -1, -1, -1, -1, 0x48, 0x85, 0xC9, 0x0F, 0x84, -1, -1, -1, -1, 0x48, 0x8B, 0x01, 0xFF, 0x50, -1, 0x84, 0xC0 });
        if (!CredentialsCheck)
        {
            // 1.10's reads the credentials from rsi (mov rcx, [rsi+180h] at 0x2E317D2).
            CredentialsCheck = Scan({ 0x48, 0x8B, 0x8E, -1, -1, -1, -1, 0x48, 0x85, 0xC9, 0x0F, 0x84, -1, -1, -1, -1, 0x48, 0x8B, 0x01, 0xFF, 0x50, -1, 0x84, 0xC0 });
        }
        const ptrdiff_t CredentialsNetId = CredentialsCheck ? *reinterpret_cast<const int32_t*>(CredentialsCheck + 3) : 0;
        RequireOffset(L"UMcpProfileGroup::CredentialsAccountId", Natives::UMcpProfileGroup_CredentialsAccountId, CredentialsNetId > static_cast<ptrdiff_t>(sizeof(void*)) ? CredentialsNetId - static_cast<ptrdiff_t>(sizeof(void*)) : 0);
        RequireSlot(L"FUniqueNetId::IsValid", Natives::FUniqueNetId_IsValidIndex, CredentialsCheck && Byte(CredentialsCheck + 21) % 8 == 0 ? Byte(CredentialsCheck + 21) / 8 : -1);
        const uintptr_t QueueForUser = CredentialsCheck ? FindRawCall(CredentialsCheck + 24, CredentialsCheck + 0x60) : 0;
        Require(L"FOnlineSubsystemMcp::QueueRequest (user)", Natives::FOnlineSubsystemMcp_QueueRequestForUser, QueueForUser ? Finder::RelativeTarget(QueueForUser, 1, 5) : 0);

        uintptr_t AccountInventoryChanged = FunctionOf(L"UFortRegisteredPlayerInfo::OnAccountInventoryChanged trying to initialize ability system actor");
        if (!AccountInventoryChanged)
        {
            // 3.2 drops that log line (3.2 0xC16BD0); the function is still the only reader of the bonus token's template
            // id, which it checks after the boost buffs (3.1 0xC02950 reads it too).
            AccountInventoryChanged = FunctionOf(L"Token:accountinventorybonus");
        }
        Require(L"UFortRegisteredPlayerInfo::OnAccountInventoryChanged", Natives::UFortRegisteredPlayerInfo_OnAccountInventoryChanged, AccountInventoryChanged);

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
        ptrdiff_t ServiceConfig = bConfigLea ? Byte(ConfigLea + 3) : 0;
        if (!bConfigLea && ClientCall)
        {
            // 1.10 (0x7C0609) loads the service, then lea rdx, name; add rcx, 18h; call.
            const uintptr_t ConfigAdd = FindInstruction(Dispatch, ClientCall, { 0x48, 0x8D, 0x15, -1, -1, -1, -1, 0x48, 0x83, 0xC1, -1, 0xE8 });
            ServiceConfig = ConfigAdd ? Byte(ConfigAdd + 10) : 0;
        }
        RequireOffset(L"FGameServiceMcp::ServiceConfig", Natives::FGameServiceMcp_ServiceConfig, ServiceConfig);
    }

    void ResolveThreadingForPerformance()
    {
        Sites::ShouldUseThreadingForPerformanceDedicatedCheck = nullptr;
        const uintptr_t OneThread = OnlyRef(L"ONETHREAD");
        const bool bParamTest = OneThread && Byte(OneThread + 7) == 0xE8 && Byte(OneThread + 12) == 0x84 && Byte(OneThread + 13) == 0xC0 && Byte(OneThread + 14) == 0x75;
        const uintptr_t OnlyOneThread = bParamTest ? OneThread + 16 + static_cast<int8_t>(Byte(OneThread + 15)) : 0;
        // 5.0 (UE 4.21) sets al and stores it after the static's lock: mov al, 1; lea rcx, [...]; mov [OnlyOneThread], al
        // (CL-4214610 0x1586792).
        const bool bStoresOne = OnlyOneThread && Byte(OnlyOneThread) == 0xB0 && Byte(OnlyOneThread + 1) == 0x01 && FindInstruction(OnlyOneThread + 2, OnlyOneThread + 0x12, { 0x88, 0x05 });
        if (OnlyOneThread && ((Byte(OnlyOneThread) == 0xC6 && Byte(OnlyOneThread + 1) == 0x05 && Byte(OnlyOneThread + 6) == 0x01) || bStoresOne))
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
        // 5.0 (UE 4.21) compares with r15b and stores r12b (CL-4214610 0x1714758): cmp [GIsClient], r15b; jnz; cmp
        // [GIsServer], r15b; jz; mov [rdi+1F0h], r12b (44 88 A7 F0 01 00 00), which FLinker::Hook rewrites to
        // mov byte ptr [rdi+1F0h], 0, the same length while the base needs no REX.B.
        Sites::LinkerFilterRegisterStore = false;
        const std::initializer_list<int32_t> RegisterFilterPattern = { -1, 0x38, -1, -1, -1, -1, -1, 0x75, -1, -1, 0x38, -1, -1, -1, -1, -1, 0x74, -1, -1, 0x88 };
        for (uintptr_t Site = Filters == 0 && IsClient && IsServer ? FindInstruction(Finder::TextBegin(), Finder::TextEnd(), RegisterFilterPattern) : 0; Site; Site = FindInstruction(Site + 1, Finder::TextEnd(), RegisterFilterPattern))
        {
            const bool bRex = (Byte(Site) & 0xF0) == 0x40 && (Byte(Site + 9) & 0xF0) == 0x40 && (Byte(Site + 18) & 0xF0) == 0x40;
            const bool bReadsFlags = bRex && (Byte(Site + 2) & 0xC7) == 0x05 && Finder::RelativeTarget(Site, 3, 7) == IsClient
                && (Byte(Site + 11) & 0xC7) == 0x05 && Finder::RelativeTarget(Site + 9, 3, 7) == IsServer;
            const bool bStoresMember = (Byte(Site + 18) & 0x01) == 0 && (Byte(Site + 20) & 0xC0) == 0x80 && (Byte(Site + 20) & 0x07) != 0x04;
            if (bReadsFlags && bStoresMember)
            {
                Sites::LinkerFilterClientButNotServer = reinterpret_cast<uint8_t*>(Site + 18);
                Sites::LinkerFilterRegisterStore = true;
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
        // 1.10's event thunk (0x1D53B10) saves rbx and reads the event's name before it stores DeltaSeconds.
        for (uintptr_t Site = GetWorldSettings && !ReceiveTickCalls ? FindInstruction(Finder::TextBegin(), Finder::TextEnd(), TickPattern) : 0; Site; Site = FindInstruction(Site + 1, Finder::TextEnd(), TickPattern))
        {
            const uintptr_t Call = Site + 24;
            const uintptr_t ReceiveTick = Finder::RelativeTarget(Call, 1, 5);
            const bool bFloatThunk = IsFunctionEntry(ReceiveTick) && FindInstruction(ReceiveTick, ReceiveTick + 0x20, { 0xF3, 0x0F, 0x11, 0x4C, 0x24, -1 });
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
        uintptr_t IncrementId = MarkDirtyEnd ? FindInstruction(MarkDirty, MarkDirtyEnd, { 0xFF, 0x41, -1 }) : 0;
        // 5.0 (UE 4.21) takes the spec array's address first: lea rbx, [rcx+460h]; ...; inc dword ptr [rbx+50h]
        // (CL-4214610 0x606235). Any base but rsp; IDCounter is the disp8 either way.
        for (uintptr_t Increment = !IncrementId && MarkDirtyEnd ? FindInstruction(MarkDirty, MarkDirtyEnd, { 0xFF }) : 0; Increment && !IncrementId; Increment = FindInstruction(Increment + 1, MarkDirtyEnd, { 0xFF }))
        {
            if ((Byte(Increment + 1) & 0xF8) == 0x40 && Byte(Increment + 1) != 0x44 && Byte(Increment - 1) != 0x41 && FindRawCall(Increment + 3, Increment + 0x40))
            {
                IncrementId = Increment;
            }
        }
        RequireOffset(L"FFastArraySerializer::IDCounter", Natives::FFastArraySerializer_IDCounter, IncrementId ? Byte(IncrementId + 2) : 0);
        const uintptr_t MarkArrayDirty = IncrementId ? FindRawCall(IncrementId, MarkDirtyEnd) : 0;
        // 5.0 (UE 4.21) inlines MarkArrayDirty: ItemMap.Reset() is the call (rcx = the serializer, ItemMap first), then
        // IncrementArrayReplicationKey, inc dword ptr [rbx+IDCounter+4], and CachedNumItems and
        // CachedNumItemsToConsiderForWriting = INDEX_NONE in one store, mov qword ptr [rbx+0A8h], -1 (CL-4214610 0x606253).
        // Core then does the same (FFastArraySerializer::MarkArrayDirty) with the Reset native and those offsets.
        Natives::FFastArraySerializer_ItemMapReset = nullptr;
        Natives::FFastArraySerializer_CachedNumItems = -1;
        if (MarkArrayDirty && IncrementId && Byte(MarkArrayDirty + 5) == 0xFF && Byte(MarkArrayDirty + 6) == Byte(IncrementId + 1) && Byte(MarkArrayDirty + 7) == Byte(IncrementId + 2) + 4)
        {
            const uint8_t CachedStore[] = { 0x48, 0xC7, static_cast<uint8_t>(0x80 | (Byte(IncrementId + 1) & 0x07)) };
            for (uintptr_t Store = MarkArrayDirty + 8; Store + 11 <= MarkArrayDirty + 0x40 && Natives::FFastArraySerializer_CachedNumItems < 0; ++Store)
            {
                if (memcmp(reinterpret_cast<const void*>(Store), CachedStore, sizeof(CachedStore)) == 0 && *reinterpret_cast<const int32_t*>(Store + 7) == -1)
                {
                    Natives::FFastArraySerializer_CachedNumItems = *reinterpret_cast<const int32_t*>(Store + 3);
                }
            }
            if (Natives::FFastArraySerializer_CachedNumItems > 0)
            {
                Natives::FFastArraySerializer_ItemMapReset = reinterpret_cast<decltype(Natives::FFastArraySerializer_ItemMapReset)>(Finder::RelativeTarget(MarkArrayDirty, 1, 5));
                Log::Info(L"Finder: FFastArraySerializer::MarkArrayDirty is inlined; ItemMap.Reset +0x{:X}, CachedNumItems +0x{:X}", Finder::RelativeTarget(MarkArrayDirty, 1, 5) - Finder::ImageBase(), Natives::FFastArraySerializer_CachedNumItems);
            }
        }
        if (!Natives::FFastArraySerializer_ItemMapReset)
        {
            Require(L"FFastArraySerializer::MarkArrayDirty", Natives::FFastArraySerializer_MarkArrayDirty, MarkArrayDirty ? Finder::RelativeTarget(MarkArrayDirty, 1, 5) : 0);
        }

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
        Sites::ShouldUseIdleModeInlinedBranch = nullptr;
        if (!IdleFalse || IdleFalse[0] != 0x32 || IdleFalse[1] != 0xC0)
        {
            // 5.0 (UE 4.21) inlines ShouldUseIdleMode into FEngineLoop::Tick (CL-4214610 0x2B6DC6): mov rax,
            // [CVarIdleWhenNotForeground]; ...; cmp dword ptr [rax], r14d; jz NotIdle; call FApp::HasFocus; test al, al;
            // jnz NotIdle; mov sil, 1 (bIdleMode). The cvar's static is where the "t.IdleWhenNotForeground" lookup
            // stores its result (mov [rip+X], rax after the call). The site only tells that the check is inlined: Core
            // turns t.IdleWhenNotForeground off through the console manager (FEngineLoop's HookIdleMode).
            for (const uintptr_t Ref : Finder::StringRefs(L"t.IdleWhenNotForeground"))
            {
                uintptr_t Static = 0;
                for (uintptr_t Store = FindInstruction(Ref, Ref + 0x40, { 0x48, 0x89, 0x05 }); Store && !Static; Store = FindInstruction(Store + 1, Ref + 0x40, { 0x48, 0x89, 0x05 }))
                {
                    Static = Finder::RelativeTarget(Store, 3, 7);
                }
                for (uintptr_t Load = Static ? FindInstruction(Finder::TextBegin(), Finder::TextEnd(), { 0x48, 0x8B, 0x05 }) : 0; Load && !Sites::ShouldUseIdleModeInlinedBranch; Load = FindInstruction(Load + 1, Finder::TextEnd(), { 0x48, 0x8B, 0x05 }))
                {
                    if (Finder::RelativeTarget(Load, 3, 7) != Static)
                    {
                        continue;
                    }
                    // cmp dword ptr [rax], reg (39 30 / 44 39 30 ...), then jz short.
                    for (uintptr_t Compare = Load + 7; Compare < Load + 0x18; ++Compare)
                    {
                        if (Byte(Compare) != 0x39 || (Byte(Compare + 1) & 0xC7) != 0x00 || Byte(Compare + 2) != 0x74)
                        {
                            continue;
                        }
                        const uintptr_t Branch = Compare + 2;
                        const uintptr_t NotIdle = Branch + 2 + static_cast<int8_t>(Byte(Branch + 1));
                        const uintptr_t FocusTest = Branch + 2;
                        const bool bFocus = Byte(FocusTest) == 0xE8 && Byte(FocusTest + 5) == 0x84 && Byte(FocusTest + 6) == 0xC0 && Byte(FocusTest + 7) == 0x75 && FocusTest + 9 + static_cast<int8_t>(Byte(FocusTest + 8)) == NotIdle;
                        if (bFocus)
                        {
                            Sites::ShouldUseIdleModeInlinedBranch = reinterpret_cast<uint8_t*>(Branch);
                            break;
                        }
                    }
                }
            }
        }
        if (Sites::ShouldUseIdleModeInlinedBranch)
        {
            Sites::ShouldUseIdleModeCheck = nullptr;
            Sites::ShouldUseIdleModeFalse = nullptr;
        }
        else if (!IdleFalse || IdleFalse[0] != 0x32 || IdleFalse[1] != 0xC0)
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
        uintptr_t Sleep = UpdateTimeEnd ? FindInstruction(UpdateTime, UpdateTimeEnd, SleepPattern) : 0;
        uintptr_t Slack = Sleep ? Sleep + 2 : 0;
        // 1.9 (CL-3757339, 0x2512664) moves the wait into xmm0 before it takes the slack off: jbe; movaps xmm0, xmm6;
        // subss xmm0, [0.002f]; call. The call is still 13 bytes in, and without the subss xmm0 is the whole wait.
        if (!Sleep && UpdateTimeEnd)
        {
            Sleep = FindInstruction(UpdateTime, UpdateTimeEnd, { 0x76, 0x10, 0x0F, 0x28, -1, 0xF3, 0x0F, 0x5C, -1, -1, -1, -1, -1, 0xE8 });
            Slack = Sleep ? Sleep + 5 : 0;
        }
        const uintptr_t SleepTarget = Sleep ? Finder::RelativeTarget(Sleep + 13, 1, 5) : 0;
        const bool bSlack = Slack && (Byte(Slack + 3) & 0xC7) == 0x05 && *reinterpret_cast<const float*>(Finder::RelativeTarget(Slack, 4, 8)) == 0.002f;
        uintptr_t Spin = 0;
        for (uintptr_t Exit = bSlack ? FindInstruction(Sleep + 18, Sleep + 0x60, { 0x73, -1, 0x0F, 0x28, -1, 0xE8 }) : 0; Exit && !Spin; Exit = FindInstruction(Exit + 1, Sleep + 0x60, { 0x73, -1, 0x0F, 0x28, -1, 0xE8 }))
        {
            Spin = Finder::RelativeTarget(Exit + 5, 1, 5) == SleepTarget ? Exit : 0;
        }
        // 1.10 (0x1D0EC1D) aligns the loop with a nop and waits on xmm8: jnb; nop; movaps xmm0, xmm8; call.
        for (uintptr_t Exit = bSlack && !Spin ? FindInstruction(Sleep + 18, Sleep + 0x60, { 0x73 }) : 0; Exit && !Spin; Exit = FindInstruction(Exit + 1, Sleep + 0x60, { 0x73 }))
        {
            uintptr_t Move = Exit + 2;
            while (Byte(Move) == 0x90 && Move < Exit + 0x12)
            {
                ++Move;
            }
            // 4.2 (0x2463C9D) aligns it with a multi-byte nop instead: nop dword ptr [rax] (0F 1F 00), or a longer form.
            uintptr_t LongNop = Move;
            while (Byte(LongNop) == 0x66 && LongNop < Move + 4)
            {
                ++LongNop;
            }
            if (Byte(LongNop) == 0x0F && Byte(LongNop + 1) == 0x1F && (Byte(LongNop + 2) & 0x38) == 0)
            {
                const uint8_t Modrm = Byte(LongNop + 2);
                const uint32_t Sib = (Modrm & 0x07) == 0x04 && (Modrm & 0xC0) != 0xC0 ? 1 : 0;
                const uint32_t Displacement = (Modrm & 0xC0) == 0x40 ? 1 : (Modrm & 0xC0) == 0x80 ? 4 : 0;
                Move = LongNop + 3 + Sib + Displacement;
            }
            Move += (Byte(Move) & 0xF0) == 0x40 ? 1 : 0;
            const bool bWait = Byte(Move) == 0x0F && Byte(Move + 1) == 0x28 && Byte(Move + 3) == 0xE8;
            Spin = bWait && Finder::RelativeTarget(Move + 3, 1, 5) == SleepTarget ? Exit : 0;
        }
        if (Spin)
        {
            Sites::MaxTickRateWait = { reinterpret_cast<uint8_t*>(Sleep), reinterpret_cast<uint8_t*>(Slack), reinterpret_cast<uint8_t*>(Spin) };
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
            // The UpdateManager load is mov rcx, [rbx+disp32] up to 5.0 and mov rcx, [rsi+disp32] on 5.10 (CL-4240749
            // 0xDE9C66): any base register.
            const std::initializer_list<int32_t> CallPattern = { 0xE8, -1, -1, -1, -1, 0x84, 0xC0, 0x75, -1, 0x48, 0x8B, -1, -1, -1, 0x00, 0x00,
                0x48, 0x85, 0xC9, 0x74, -1, 0x48, 0x8B, 0x01, 0x33, 0xD2, 0xFF, 0x90 };
            // rbx alone first, as every build before 5.10 has it; any base only if that doesn't find exactly one.
            const std::vector<uintptr_t> Calls = Finder::CallsTo(DedicatedInstance, false);
            for (const bool bAnyBase : { false, true })
            {
                if (bAnyBase && StartChecks == 1)
                {
                    break;
                }
                StartChecks = 0;
                for (const uintptr_t Call : Calls)
                {
                    const uintptr_t Branch = Call + 7;
                    const uintptr_t Target = Branch + 2 + static_cast<int8_t>(Byte(Branch + 1));
                    const bool bLoadsUpdateManager = bAnyBase ? (Byte(Call + 11) & 0xF8) == 0x88 && (Byte(Call + 11) & 0x07) != 0x04 : Byte(Call + 11) == 0x8B;
                    if (FindInstruction(Call, Call + 28, CallPattern) == Call && bLoadsUpdateManager && Call + 21 + static_cast<int8_t>(Byte(Call + 20)) == Target)
                    {
                        StartBranch = Branch;
                        StartTarget = Target;
                        ++StartChecks;
                    }
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

        // A packed build resolves it first and swaps it out of the vtables this search goes by (Server Boot.cpp).
        int32_t IsDedicatedMatches = Natives::FOnlineSubsystemImpl_IsDedicated ? 1 : 0;
        const uintptr_t IsDedicated = Natives::FOnlineSubsystemImpl_IsDedicated ? reinterpret_cast<uintptr_t>(Natives::FOnlineSubsystemImpl_IsDedicated) : Finder::FindOnlineSubsystemIsDedicated(IsDedicatedMatches);
        Require(L"FOnlineSubsystemImpl::IsDedicated", Natives::FOnlineSubsystemImpl_IsDedicated, IsDedicatedMatches == 1 ? IsDedicated : 0);
        RequireOffset(L"FOnlineSubsystemImpl::bForceDedicated", Natives::FOnlineSubsystemImpl_bForceDedicated, IsDedicatedMatches == 1 ? OnlineSubsystemForceDedicatedOffset(IsDedicated) : 0);
    }
}

uintptr_t Finder::FindOnlineSubsystemIsDedicated(int32_t& OutMatches)
{
    // FOnlineSubsystemImpl::IsDedicated: xor eax, eax; cmp [rcx+bForceDedicated], al; setnz al; ret, with SetForceDedicated
    // (mov [rcx+bForceDedicated], dl; ret) two slots on in the same vtables. 5.0 (UE 4.21) compiles it as
    // cmp byte ptr [rcx+bForceDedicated], 0; setnz al; ret (0x3E5400 on CL-4204761).
    uintptr_t IsDedicated = 0;
    OutMatches = 0;
    for (const std::initializer_list<int32_t> IsDedicatedPattern : { std::initializer_list<int32_t>{ 0x33, 0xC0, 0x38, 0x81, -1, -1, -1, -1, 0x0F, 0x95, 0xC0, 0xC3 },
                                                                     std::initializer_list<int32_t>{ 0x80, 0xB9, -1, -1, -1, -1, 0x00, 0x0F, 0x95, 0xC0, 0xC3 } })
    {
        for (uintptr_t Getter = FindInstruction(Finder::TextBegin(), Finder::TextEnd(), IsDedicatedPattern); Getter; Getter = FindInstruction(Getter + 1, Finder::TextEnd(), IsDedicatedPattern))
        {
            const int32_t Offset = OnlineSubsystemForceDedicatedOffset(Getter);
            for (void** Entry : VTablesWithEntry(Getter))
            {
                const uintptr_t Setter = reinterpret_cast<uintptr_t>(Entry[2]);
                if (IsInText(Setter) && Byte(Setter) == 0x88 && Byte(Setter + 1) == 0x91 && *reinterpret_cast<const int32_t*>(Setter + 2) == Offset && Byte(Setter + 6) == 0xC3)
                {
                    if (Getter != IsDedicated)
                    {
                        ++OutMatches;
                    }
                    IsDedicated = Getter;
                }
            }
        }
        if (IsDedicated)
        {
            break;
        }
    }
    return IsDedicated;
}

bool Finder::ResolveOnlineSubsystemIsDedicated()
{
    if (Natives::FOnlineSubsystemImpl_IsDedicated && Natives::FOnlineSubsystemImpl_bForceDedicated >= 0)
    {
        return true;
    }
    int32_t Matches = 0;
    const uintptr_t IsDedicated = FindOnlineSubsystemIsDedicated(Matches);
    if (Matches != 1)
    {
        Log::Error(L"Finder: FOnlineSubsystemImpl::IsDedicated not found ({} matches)", Matches);
        return false;
    }
    Natives::FOnlineSubsystemImpl_IsDedicated = reinterpret_cast<decltype(Natives::FOnlineSubsystemImpl_IsDedicated)>(IsDedicated);
    Natives::FOnlineSubsystemImpl_bForceDedicated = OnlineSubsystemForceDedicatedOffset(IsDedicated);
    return true;
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

namespace
{
    // The vtable of the delegate instance AFortPlayerControllerOutpost::ServerAbortDisintegration (4.1 0xFC1810, 4.2
    // 0xF798F0) sends its MCP command's completion with: after the completion lambda's address (its first lea of code),
    // the functor base's vtable, then the instance's own (lea rax, vtable; mov [rbx], rax, twice).
    uintptr_t OutpostMcpRequestVTable(uintptr_t Abort, uintptr_t AbortEnd, uintptr_t& OutCompleted)
    {
        OutCompleted = 0;
        int32_t DataLoads = 0;
        for (uintptr_t Site = Abort; Site && Site + 7 <= AbortEnd; ++Site)
        {
            if (Byte(Site) != 0x48 || Byte(Site + 1) != 0x8D || Byte(Site + 2) != 0x05)
            {
                continue;
            }
            const uintptr_t Target = Finder::RelativeTarget(Site, 3, 7);
            if (IsInText(Target))
            {
                OutCompleted = OutCompleted ? OutCompleted : Target;
            }
            else if (OutCompleted && ++DataLoads == 2)
            {
                return Target;
            }
        }
        return 0;
    }

    // The build's NUM_DELEGATE_INLINE_BYTES (Natives::NumDelegateInlineBytes, which FDelegateBase and TMulticastDelegate
    // read), for the server: the client reads it from its console delegates, which the server never resolves. The MCP
    // request delegate's CreateCopy (IBaseDelegateInstance::CreateCopy(FDelegateBase& Base): new (Base) T(*this))
    // allocates in Base. Up to 4.1 it calls FDelegateBase::Allocate, whose DelegateSize follows TInlineAllocator<2>'s
    // two inline elements and SecondaryData (4.1 0x29762F0 calling 0x2A2C40: cmp dword [rcx+30h], 0); 4.2 inlines
    // Allocate, with DelegateSize right after FHeapAllocator's Data (0x29824B0: cmp dword [rdx+8], 0; ...; mov dword
    // [rbx+8], 3), UE 4.21's NUM_DELEGATE_INLINE_BYTES 0. Only that shape switches to the heap layout; anything else
    // keeps the inline one (32).
    void ResolveDelegateLayout()
    {
        const uintptr_t Abort = FunctionOf(L"AFortPlayerControllerOutpost::ServerAbortDisintegration");
        const uintptr_t AbortEnd = Abort ? Finder::FunctionEnd(Abort) : 0;
        uintptr_t Completed = 0;
        void** const VTable = reinterpret_cast<void**>(OutpostMcpRequestVTable(Abort, AbortEnd, Completed));
        // The same instance's GetHandle (mov rax, [rcx+Handle]; mov [rdx], rax; mov rax, rdx; ret) gives the instance
        // vtable's layout (Natives::IDelegateInstance_GetHandleIndex), which the client reads from its console
        // delegates: slot 8 up to 2.4.2, 4 from 2.5 (UE 4.20), 5 from 5.30. The server's own instances (Core's timer
        // delegates, FTimerManager) take the build's layout from it.
        int32_t GetHandleIndex = -1;
        for (int32_t Index = 0; VTable && Index < 0x10 && GetHandleIndex < 0; ++Index)
        {
            const uintptr_t Function = VTableEntry(VTable, Index);
            if (Function && FindInstruction(Function, Function + 11, { 0x48, 0x8B, 0x41, -1, 0x48, 0x89, 0x02, 0x48, 0x8B, 0xC2, 0xC3 }) == Function)
            {
                GetHandleIndex = Index;
            }
        }
        if (GetHandleIndex == 4 || GetHandleIndex == 5 || GetHandleIndex == 8)
        {
            Natives::IDelegateInstance_GetHandleIndex = GetHandleIndex;
            Natives::IDelegateInstance_GetHandleIndexResolved = true;
            Log::Info(L"Finder: IDelegateInstance::GetHandle is slot {} (from the MCP request delegate)", GetHandleIndex);
        }
        else
        {
            Log::Warning(L"Finder: IDelegateInstance::GetHandle not found in the MCP request delegate (slot {}), keeping slot {}", GetHandleIndex, Natives::IDelegateInstance_GetHandleIndex);
        }
        // The handle the request's delegate gets: movups [frame+10h], xmm; call FDelegateHandle::GenerateNewID (5.40
        // 0x120D62A calling 0x1583270: lock xadd on the engine's counter). Core's own instances take their handles from it
        // too (FDelegateHandle::GenerateNewID), so an engine Remove(Handle) never matches one of Core's in a delegate both
        // bind. Not found keeps Core's own counter.
        const uintptr_t HandleStore = Abort ? FindInstruction(Abort, AbortEnd, { 0x0F, 0x11, -1, 0x10, 0xE8 }) : 0;
        const uintptr_t GenerateNewID = HandleStore ? Finder::RelativeTarget(HandleStore + 4, 1, 5) : 0;
        if (IsInText(GenerateNewID))
        {
            Natives::FDelegateHandle_GenerateNewID = reinterpret_cast<decltype(Natives::FDelegateHandle_GenerateNewID)>(GenerateNewID);
            Log::Info(L"Finder: FDelegateHandle::GenerateNewID 0x{:X}, Core's delegate handles come from the engine's counter", GenerateNewID - Finder::ImageBase());
        }
        else
        {
            Log::Warning(L"Finder: FDelegateHandle::GenerateNewID not found in the MCP request; Core's delegate handles come from its own counter");
        }
        uintptr_t CreateCopy = 0;
        for (int32_t Index = 0; VTable && Index < 0x10 && !CreateCopy; ++Index)
        {
            const uintptr_t Function = VTableEntry(VTable, Index);
            uintptr_t SizeCheck = IsInText(Function) ? FindInstruction(Function, Function + 0x20, { 0x83, 0x7A, 0x08, 0x00 }) : 0;
            uintptr_t SizeWindow = 0x40;
            if (!SizeCheck && IsInText(Function))
            {
                // 5.0 (UE 4.21) loads it first: mov eax, [rdx+8]; ...; test eax, eax (CL-4214610 0x2ED93A).
                const uintptr_t SizeLoad = FindInstruction(Function, Function + 0x20, { 0x8B, 0x42, 0x08 });
                SizeCheck = SizeLoad && FindInstruction(SizeLoad + 3, SizeLoad + 0x10, { 0x85, 0xC0 }) ? SizeLoad : 0;
                // Its DelegateSize store is 0x3B on (0x2ED975), past the 0x40 the older shape needs.
                SizeWindow = 0x60;
            }
            const uintptr_t SizeStore = SizeCheck ? FindInstruction(SizeCheck, SizeCheck + SizeWindow, { 0xC7, -1, 0x08, 0x03, 0x00, 0x00, 0x00 }) : 0;
            CreateCopy = SizeStore && (Byte(SizeStore + 1) & 0xF8) == 0x40 ? Function : 0;
        }
        if (CreateCopy)
        {
            Natives::NumDelegateInlineBytes = 0;
            Log::Info(L"Finder: delegates use the heap allocator (NUM_DELEGATE_INLINE_BYTES 0: CreateCopy 0x{:X} keeps DelegateSize at +8)", CreateCopy - Finder::ImageBase());
            return;
        }
        Log::Info(L"Finder: delegates use the inline allocator (NUM_DELEGATE_INLINE_BYTES {})", Natives::NumDelegateInlineBytes);
    }
}

bool Finder::ResolveServer()
{
    MissingCount = 0;

    ResolveDelegateLayout();
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

    // The ClientStayWithParty RPC stub: the AFortPlayerController slot whose small body loads the function's name global
    // (mov rdx, [rip+Name]), which its static FName initializer fills (lea rdx, L"ClientStayWithParty"; lea rcx, Name).
    // Slot 0x29F on 1.1 and 1.2, 0x2D0 on 1.7.2, 0x2F2 on 3.3, 0x2FE on 3.5.1 to 4.0.
    int32_t ClientStayWithPartySlot()
    {
        const uintptr_t NameInit = First(Finder::StringRefs(L"ClientStayWithParty"));
        const uintptr_t NameStore = NameInit ? FindInstruction(NameInit + 7, NameInit + 0x20, { 0x48, 0x8D, 0x0D }) : 0;
        const uintptr_t NameGlobal = NameStore ? Finder::RelativeTarget(NameStore, 3, 7) : 0;
        return NameGlobal ? FirstVTableIndexWhere(CdoVTable(L"/Script/FortniteGame.FortPlayerController"), [NameGlobal](uintptr_t Entry)
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
    }

    // AFortGameMode::SendStayWithParty, the function the game mode zone's zone end calls within its first 0x200 bytes
    // (3.3 0xBD1530, 3.5.1 0xC0B8C0, 3.5.2 0xC10710, 3.6 0xC18430, 4.0 0xC45E50): the call whose target makes the
    // ClientStayWithParty virtual call (3.6 0xBE4B80, 4.0 0xC1F230). 0 when the zone end doesn't call it.
    uintptr_t ZoneEndStayWithParty(uintptr_t ZoneEnd, int32_t StaySlot)
    {
        const uintptr_t ZoneEndEnd = ZoneEnd && StaySlot >= 0 && Finder::FunctionStart(ZoneEnd) == ZoneEnd ? Finder::FunctionEnd(ZoneEnd) : 0;
        const uintptr_t Window = ZoneEndEnd ? std::min<uintptr_t>(ZoneEndEnd, ZoneEnd + 0x200) : 0;
        const int32_t StayCall = StaySlot * 8;
        for (uintptr_t Call = Window ? FindRawCall(ZoneEnd, Window) : 0; Call; Call = FindRawCall(Call + 1, Window))
        {
            const uintptr_t Target = Finder::RelativeTarget(Call, 1, 5);
            const uintptr_t TargetEnd = Finder::FunctionStart(Target) == Target ? Finder::FunctionEnd(Target) : 0;
            if (TargetEnd && FindInstruction(Target, TargetEnd, { 0xFF, 0x90, StayCall & 0xFF, (StayCall >> 8) & 0xFF, (StayCall >> 16) & 0xFF, 0x00 }))
            {
                return Target;
            }
        }
        return 0;
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
    // mov r32, [rcx+Displacement]; 2.5's (0x1268A50) sign-extends MinAlignment instead: movsxd r10, [rcx+44h].
    // 5.0 (UE 4.21, CL-4214610 0x165BC00) adds PropertiesSize straight in: add r13d, [rcx+40h]; tried when no thunk reads
    // both with a mov.
    bool bAddReads = false;
    const auto ReadsStruct = [&bAddReads](uintptr_t Begin, uintptr_t End, int32_t Displacement)
    {
        for (const int32_t Opcode : { 0x8B, 0x63, bAddReads ? 0x03 : 0x8B })
        {
            for (uintptr_t Load = FindInstruction(Begin, End, { Opcode, -1, Displacement }); Load; Load = FindInstruction(Load + 1, End, { Opcode, -1, Displacement }))
            {
                if ((Byte(Load + 1) & 0xC7) == 0x41)
                {
                    return true;
                }
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
        // 5.0 (UE 4.21) encodes the tail jump through r9 with REX 41 instead of 49 (CL-4214610 0x17765AE).
        if (IsInText(Thunk) && FindInstruction(Thunk, Thunk + 0x14, { 0x48, 0x8B, 0x41, StructOffset, 0x44, 0x8B, 0x41, ArrayDimOffset, 0x48, 0x8B, 0xC8, 0x4C, 0x8B, 0x08, -1, 0xFF, 0xA1 }) == Thunk
            && (Byte(Thunk + 14) == 0x49 || Byte(Thunk + 14) == 0x41))
        {
            StructSlots.push_back(*reinterpret_cast<const int32_t*>(Thunk + 17) / static_cast<int32_t>(sizeof(void*)));
        }
    }
    if (StructSlots.size() == 2)
    {
        Natives::UStruct_InitializeStructIndex = std::min(StructSlots[0], StructSlots[1]);
        Natives::UStruct_DestroyStructIndex = std::max(StructSlots[0], StructSlots[1]);
    }

    for (const bool bAdd : { false, true })
    {
        bAddReads = bAdd;
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
    // 5.0 (UE 4.21) compares with a zeroed register: cmp [rdx+20h], rdi (48 39 7A 20; CL-4214610 0x287C4AF).
    for (uintptr_t Compare = !Code && ThunkEnd ? FindInstruction(Thunk, ThunkEnd, { 0x48, 0x39 }) : 0; Compare && !Code; Compare = FindInstruction(Compare + 1, ThunkEnd, { 0x48, 0x39 }))
    {
        if ((Byte(Compare + 2) & 0xC7) == 0x42)
        {
            Code = Byte(Compare + 3);
        }
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
    // Epic's FFrame declares Node, Object, Code, Locals in a row (5.40: +0x10, +0x18, +0x20, +0x28).
    Natives::FFrame_Locals = Code + static_cast<ptrdiff_t>(sizeof(void*));
    Natives::FFrame_PropertyChainForCompiledIn = PropertyChain;
    Natives::FFrame_Step = reinterpret_cast<decltype(Natives::FFrame_Step)>(Step);
    Natives::FFrame_StepExplicitProperty = reinterpret_cast<decltype(Natives::FFrame_StepExplicitProperty)>(StepExplicitProperty);
    Log::Info(L"Finder: FFrame Object +0x{:X}, Code +0x{:X}, Locals +0x{:X}, PropertyChainForCompiledIn +0x{:X}", Object, Code, Natives::FFrame_Locals, PropertyChain);
    return true;
}

namespace
{
    // Looks Name up in what may be a TMap<FName, UFunction*> at Map the way TSet::FindId does (the hash is the FName's
    // ComparisonIndex + Number, the elements { Key, Value, HashNextId, HashIndex } 0x18 bytes apart), checking each read
    // against the map's own counts first. Returns its value, or null; a read that faults (not a map) is null too.
    UFunction* ProbeFunctionMap(const uint8_t* Map, uint64_t Name)
    {
        __try
        {
            const uint8_t* const Data = *reinterpret_cast<const uint8_t* const*>(Map);
            const int32_t ArrayNum = *reinterpret_cast<const int32_t*>(Map + 0x08);
            const int32_t ArrayMax = *reinterpret_cast<const int32_t*>(Map + 0x0C);
            const int32_t NumFreeIndices = *reinterpret_cast<const int32_t*>(Map + 0x34);
            const int32_t* const InlineHash = reinterpret_cast<const int32_t*>(Map + 0x38);
            const int32_t* const SecondaryHash = *reinterpret_cast<const int32_t* const*>(Map + 0x40);
            const int32_t HashSize = *reinterpret_cast<const int32_t*>(Map + 0x48);
            if (!Data || ArrayNum <= 0 || ArrayNum > ArrayMax || ArrayMax > 0x10000 || NumFreeIndices < 0 || NumFreeIndices > ArrayNum
                || HashSize <= 0 || HashSize > 0x10000 || (HashSize & (HashSize - 1)) != 0 || (HashSize > 1) != (SecondaryHash != nullptr))
            {
                return nullptr;
            }
            const int32_t* const Hash = SecondaryHash ? SecondaryHash : InlineHash;
            const uint32_t KeyHash = static_cast<uint32_t>(Name) + static_cast<uint32_t>(Name >> 32);
            int32_t Id = Hash[KeyHash & (HashSize - 1)];
            for (int32_t Steps = 0; Id != -1 && Steps <= ArrayNum; ++Steps)
            {
                if (Id < 0 || Id >= ArrayNum)
                {
                    return nullptr;
                }
                const uint8_t* const Element = Data + Id * 0x18;
                if (*reinterpret_cast<const uint64_t*>(Element) == Name)
                {
                    return *reinterpret_cast<UFunction* const*>(Element + 0x08);
                }
                Id = *reinterpret_cast<const int32_t*>(Element + 0x10);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        return nullptr;
    }

    // Whether every function Class declares (its own Children, delegate signatures aside) is at Offset's map under its name.
    bool HasFunctionMapAt(const UClass* Class, int32_t Offset)
    {
        const uint8_t* const Map = reinterpret_cast<const uint8_t*>(Class) + Offset;
        int32_t Functions = 0;
        for (UField* Field = Class ? Class->Children : nullptr; Field; Field = Field->Next)
        {
            const UClass* FieldClass = Field->GetClass();
            if (!FieldClass->HasAnyCastFlag(CASTCLASS_UFunction) || FieldClass->HasAnyCastFlag(CASTCLASS_UDelegateFunction))
            {
                continue;
            }
            FName Name = Field->GetFName();
            uint64_t Key = 0;
            memcpy(&Key, &Name, sizeof(Key));
            if (ProbeFunctionMap(Map, Key) != reinterpret_cast<UFunction*>(Field))
            {
                return false;
            }
            ++Functions;
        }
        return Functions >= 3;
    }

    // UFunction's NumParms, ParmsSize and ReturnValueOffset, from KismetSystemLibrary::IsValidClass (Class, then the bool
    // return value: 2 parameters, 9 bytes, return value at +8). With a RepOffset after FunctionFlags they sit at +0x8E,
    // +0x90 and +0x92 (ObjectLayout's defaults); 2.4.2, 3.3 and 5.40 have none and, as 21.40, have them right after
    // FunctionFlags: +0x8C, +0x8E, +0x90 (5.40's K2_SetTimer reads ParmsSize at +0x8E, 0x2594090). Only the server reads
    // this (Core's natives are made there); the client keeps the defaults it has always used.
    bool DetectFunctionLayout()
    {
        const UObject* IsValidClass = StaticFindObject(nullptr, nullptr, L"/Script/Engine.KismetSystemLibrary:IsValidClass", false);
        const uint8_t* const Function = reinterpret_cast<const uint8_t*>(IsValidClass);
        const auto Matches = [Function](int32_t NumParms)
        {
            return Function[NumParms] == 2 && *reinterpret_cast<const uint16_t*>(Function + NumParms + 2) == 9 && *reinterpret_cast<const uint16_t*>(Function + NumParms + 4) == 8;
        };
        const int32_t FunctionFlags = ObjectLayout::Current.UFunction_FunctionFlags;
        for (const int32_t NumParms : { FunctionFlags + 6, FunctionFlags + 4 })
        {
            if (Function && Matches(NumParms))
            {
                ObjectLayout::Current.UFunction_NumParms = NumParms;
                ObjectLayout::Current.UFunction_ParmsSize = NumParms + 2;
                ObjectLayout::Current.UFunction_ReturnValueOffset = NumParms + 4;
                return true;
            }
        }
        return false;
    }
}

bool Finder::ResolveClassFunctionMap()
{
    // UClass::FuncMap, the TMap<FName, UFunction*> UClass::FindFunctionByName looks a name up in first (5.40 0x170A3F0
    // reads it at +0x100, its super classes' cache, SuperFuncMap, at +0x150). It follows ClassDefaultObject (5.40 +0xF8).
    // The engine finds a Blueprint delegate's function through it, so Core's natives go in it (UClass::AddNativeFunction).
    // Checked on classes whose every own function must be in it under its name.
    static int32_t Resolved = 0;
    if (Resolved != 0)
    {
        return Resolved > 0;
    }
    Resolved = -1;

    if (!DetectFunctionLayout())
    {
        Log::Error(L"Finder: UFunction's ParmsSize not found (KismetSystemLibrary::IsValidClass); Core can't bind its natives to Blueprint delegates");
        return false;
    }

    const UClass* Classes[] = {
        FindObject<UClass>(nullptr, L"/Script/Engine.Actor"),
        FindObject<UClass>(nullptr, L"/Script/Engine.KismetSystemLibrary"),
        FindObject<UClass>(nullptr, L"/Script/Engine.PlayerController"),
    };
    const int32_t First = ObjectLayout::Current.UClass_ClassDefaultObject + static_cast<int32_t>(sizeof(void*));
    for (int32_t Offset = First; Offset < First + 0x60 && Resolved < 0; Offset += static_cast<int32_t>(sizeof(void*)))
    {
        if (std::all_of(std::begin(Classes), std::end(Classes), [Offset](const UClass* Class) { return Class && HasFunctionMapAt(Class, Offset); }))
        {
            ObjectLayout::Current.UClass_FuncMap = Offset;
            Resolved = 1;
        }
    }
    if (Resolved < 0)
    {
        Log::Error(L"Finder: UClass::FuncMap not found after ClassDefaultObject (+0x{:X}); Core can't bind its natives to Blueprint delegates", ObjectLayout::Current.UClass_ClassDefaultObject);
        return false;
    }
    Log::Info(L"Finder: UClass::FuncMap at +0x{:X}, UFunction::ParmsSize at +0x{:X}: Core's natives bind to Blueprint delegates", ObjectLayout::Current.UClass_FuncMap, ObjectLayout::Current.UFunction_ParmsSize);
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

    uintptr_t ManagerLoad = FindText ? FindInstruction(FindText, FindText + 0x40, { 0x48, 0x8B, 0x05 }) : 0;
    const uintptr_t InlinedDisplayCall = FindText ? FindInstruction(FindText, FindText + 0x100, { 0x4C, 0x8B, 0xCF, 0x48, 0x89, 0x5C, 0x24, 0x20, 0x4C, 0x8B, 0xC6, 0x48, 0x8D, 0x55, -1, 0x48, 0x8B, 0xC8, 0xE8 }) : 0;
    uintptr_t DisplayCall = InlinedDisplayCall ? InlinedDisplayCall + 18 : 0;

    // 2.2 no longer inlines FTextLocalizationManager::Get (2.2 0xBD4470: sub rsp, 28h; mov rax, cs:Manager): FindText calls
    // it first, moves its result to rcx (mov rcx, rax) and calls FindDisplayString next (2.2 0xB9E868, 0xB9E87F). Before,
    // with no Get to read, Core's Get dereferenced a null manager and the console crashed listing the show flags (#194).
    if (FindText && !DisplayCall)
    {
        const uintptr_t GetCall = FindRawCall(FindText, FindText + 0x40);
        const uintptr_t Get = GetCall ? Finder::RelativeTarget(GetCall, 1, 5) : 0;
        if (Get && Finder::BodyIs(Get, { 0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0x05 }) && Byte(GetCall + 5) == 0x48 && Byte(GetCall + 6) == 0x8B && Byte(GetCall + 7) == 0xC8)
        {
            ManagerLoad = Get + 4;
            DisplayCall = FindRawCall(GetCall + 5, GetCall + 0x40);
        }
    }

    // 5.0 (UE 4.21) inlines both Get and FindDisplayString into FindText, and Get now creates the manager on first use
    // (5.0 CL-4204761 0x1549450: mov rdi, cs:Manager; test rdi, rdi; jnz; mov ecx, 160h; call FMemory::Malloc). With
    // no rax load the manager was left null and Core's Get dereferenced it when the console listed the show flags (#343).
    // 5.0 has no FindDisplayString of its own to call, so only the manager is kept there.
    for (uintptr_t Address = FindText; FindText && !ManagerLoad && Address < FindText + 0x40; ++Address)
    {
        if (Byte(Address) == 0x48 && Byte(Address + 1) == 0x8B && (Byte(Address + 2) & 0xC7) == 0x05)
        {
            ManagerLoad = Address;
        }
    }

    if (ManagerLoad)
    {
        Natives::GTextLocalizationManager = reinterpret_cast<FTextLocalizationManager**>(Finder::RelativeTarget(ManagerLoad, 3, 7));
    }

    if (!ManagerLoad || !DisplayCall)
    {
        Log::Error(L"Finder: FTextLocalizationManager::FindDisplayString not found");
        return false;
    }

    Natives::FTextLocalizationManager_FindDisplayString = reinterpret_cast<decltype(Natives::FTextLocalizationManager_FindDisplayString)>(Finder::RelativeTarget(DisplayCall, 1, 5));
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
    // APlayerController::AddCheats (4.1 0x243DDF0, 4.2 0x22E9F80): if no CheatManager and CheatClass is a cheat manager,
    // and the game mode's AllowCheats or bForce, a new CheatClass and its InitCheatManager. AFortPlayerController's
    // override was Super::AddCheats(bForce) up to 4.1 (0xF56D50, a jump to it); 4.2 compiles its body out (0x4D4750, the
    // shared empty function), so no Fort controller gets a cheat manager. APlayerController::AddCheats's call of it calls
    // the engine's own as that override's Super when the override made none.
    const uintptr_t AddCheats = VTableEntry(CdoVTable(L"/Script/Engine.PlayerController"), Natives::APlayerController_AddCheatsIndex);
    Natives::APlayerController_AddCheats = IsInText(AddCheats) ? reinterpret_cast<decltype(Natives::APlayerController_AddCheats)>(AddCheats) : nullptr;
    Log::Info(L"Finder: APlayerController::EnableCheats is slot 0x{:X}, AddCheats slot 0x{:X} (0x{:X})", EnableCheats, Natives::APlayerController_AddCheatsIndex,
        AddCheats ? AddCheats - Finder::ImageBase() : 0);
    return true;
}

bool Finder::ResolvePlayerControllerConsoleCommand(uintptr_t ServerCheatImplementation)
{
    if (Natives::APlayerController_ConsoleCommandIndex >= 0)
    {
        return true;
    }

    // The native AFortPlayerController::ServerCheat_Implementation is "if (CheatManager) ClientMessage(ConsoleCommand(Msg,
    // true))" (Cert 0x767B50, 1.2 0x14075E4A0, 1.7.2 0x825400): its only virtual call is ConsoleCommand.
    const uintptr_t Call = IsInText(ServerCheatImplementation) && !IsSharedEmptyBody(ServerCheatImplementation) ? FindVirtualCall(ServerCheatImplementation, ServerCheatImplementation + 0x40) : 0;
    int32_t Index = VirtualSlot(Call);

    // 2.2 strips ServerCheat_Implementation (its slot holds a jump to the shared empty function). There the slot comes
    // from UKismetSystemLibrary::ExecuteConsoleCommand (0x1A67CF0), which execExecuteConsoleCommand calls:
    // "TargetPC->ConsoleCommand(Command, true)", mov r9b, 1; ...; call [r10+disp32].
    const UFunction* Execute = Index < 0 ? FindObject<UFunction>(nullptr, L"/Script/Engine.KismetSystemLibrary:ExecuteConsoleCommand") : nullptr;
    const uintptr_t Thunk = Execute ? reinterpret_cast<uintptr_t>(Execute->GetNativeFunc()) : 0;
    const uintptr_t ThunkEnd = IsInText(Thunk) ? Finder::FunctionEnd(Thunk) : 0;
    for (const auto& [Site, Target] : ThunkEnd ? Finder::DirectCalls(Thunk, ThunkEnd) : std::vector<std::pair<uintptr_t, uintptr_t>>())
    {
        const uintptr_t WriteToLog = IsInText(Target) ? FindInstruction(Target, Target + 0x40, { 0x41, 0xB1, 0x01 }) : 0;
        Index = WriteToLog ? VirtualSlot(FindVirtualCall(WriteToLog, WriteToLog + 0x10)) : -1;
        if (Index >= 0)
        {
            break;
        }
    }
    if (Index < 0)
    {
        Log::Error(L"Finder: APlayerController::ConsoleCommand not found in ServerCheat_Implementation or ExecuteConsoleCommand");
        return false;
    }

    Natives::APlayerController_ConsoleCommandIndex = Index;
    Log::Info(L"Finder: APlayerController::ConsoleCommand is slot 0x{:X}", Index);
    return true;
}

bool Finder::ResolveAssetManagerLive()
{
    if (Natives::UAssetManager_GetPrimaryAssetDataListIndex >= 0)
    {
        return true;
    }

    // UFortCheatManager::ListWeapons (1.7.2 0x7D8750, 1.8 0x7F04D0; its exec tail-jumps to it) is UFortAssetManager::Get()
    // then the manager's item type lister (0x53EAD0, 0x551C30) for weapons ("mov dl, 0Fh; call"), whose only virtual
    // call is the asset manager's GetPrimaryAssetDataList: call [reg+0x260] on both. Only the mov's opcode is matched:
    // 1.8.1 inserts EFortItemType::Consumable at 6, so its ListWeapons (0x7FF230) passes Weapon as 10h to its lister
    // (0x535C60), which calls the same slot.
    const UFunction* Function = FindObject<UFunction>(nullptr, L"/Script/FortniteGame.FortCheatManager:ListWeapons");
    const uintptr_t Exec = Function ? reinterpret_cast<uintptr_t>(Function->GetNativeFunc()) : 0;
    const uintptr_t Jump = IsInText(Exec) ? FindInstruction(Exec, Exec + 0x20, { 0xE9 }) : 0;
    const uintptr_t List = Jump ? Finder::RelativeTarget(Jump, 1, 5) : 0;
    const uintptr_t ListEnd = IsInText(List) && Finder::FunctionStart(List) == List ? Finder::FunctionEnd(List) : 0;
    uintptr_t ItemsOfType = 0;
    for (const auto& [Call, Target] : ListEnd ? Finder::DirectCalls(List, std::min(ListEnd, List + 0x80)) : std::vector<std::pair<uintptr_t, uintptr_t>>())
    {
        if (Byte(Call - 2) == 0xB2)
        {
            ItemsOfType = Target;
            break;
        }
    }
    const uintptr_t ItemsOfTypeEnd = ItemsOfType && Finder::FunctionStart(ItemsOfType) == ItemsOfType ? Finder::FunctionEnd(ItemsOfType) : 0;
    int32_t Index = ItemsOfTypeEnd ? VirtualSlot(FindVirtualCall(ItemsOfType, ItemsOfTypeEnd)) : -1;
    if (Index < 0 && !ItemsOfType)
    {
        // 4.2 strips ListWeapons (0x4D4750 jumps straight back to its caller). UKismetSystemLibrary::GetPrimaryAssetIdList
        // (4.2 0x215F0D0, called by its exec) is UAssetManager::GetIfValid() then the manager's GetPrimaryAssetIdList:
        // call [r9+2C0h] on 4.1 and 4.2, ten slots after GetPrimaryAssetDataList (UE 4.20's AssetManager.h puts
        // GetPrimaryAssetObject, the object, path and id-for lookups and GetPrimaryAssetIdForFile between them; 4.1's
        // lister calls 0x270, its id list 0x2C0).
        const UFunction* IdList = FindObject<UFunction>(nullptr, L"/Script/Engine.KismetSystemLibrary:GetPrimaryAssetIdList");
        const uintptr_t IdListExec = IdList ? reinterpret_cast<uintptr_t>(IdList->GetNativeFunc()) : 0;
        const uintptr_t IdListExecEnd = IsInText(IdListExec) ? Finder::FunctionEnd(IdListExec) : 0;
        for (const auto& [Call, Target] : IdListExecEnd ? Finder::DirectCalls(IdListExec, IdListExecEnd) : std::vector<std::pair<uintptr_t, uintptr_t>>())
        {
            const uintptr_t GetIfValid = IsInText(Target) ? FindRawCall(Target, Target + 0x18) : 0;
            const uintptr_t Test = GetIfValid ? GetIfValid + 5 : 0;
            const bool bManager = Test && Byte(Test) == 0x48 && Byte(Test + 1) == 0x85 && Byte(Test + 2) == 0xC0 && Byte(Test + 3) == 0x74;
            const int32_t IdListIndex = bManager ? VirtualSlot(FindVirtualCall(Test, Test + 0x20)) : -1;
            if (IdListIndex >= 0x2A)
            {
                Index = IdListIndex - 10;
                ItemsOfType = Target;
                break;
            }
        }
    }
    if (Index < 0x20 || Index > 0x100)
    {
        Log::Error(L"Finder: UAssetManager::GetPrimaryAssetDataList not found (ListWeapons 0x{:X}, item type lister 0x{:X}, slot {})", List ? List - Finder::ImageBase() : 0, ItemsOfType ? ItemsOfType - Finder::ImageBase() : 0, Index);
        return false;
    }

    Natives::UAssetManager_GetPrimaryAssetDataListIndex = Index;
    Log::Info(L"Finder: UAssetManager::GetPrimaryAssetDataList is slot 0x{:X} (item type lister +0x{:X})", Index, ItemsOfType - Finder::ImageBase());
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

    // GetNetPriority and GetReplayPriority return Time * NetPriority ("mulss xmm, [reg+NetPriority]").
    const UProperty* PriorityProperty = AActor::StaticClass() ? AActor::StaticClass()->FindPropertyByName(FName(L"NetPriority", FNAME_Find)) : nullptr;
    const int32_t PriorityOffset = PriorityProperty ? PriorityProperty->GetOffset_ForInternal() : -1;
    const auto ScalesByPriority = [PriorityOffset](uintptr_t Function, uintptr_t End)
    {
        for (uintptr_t Site = Function; Function && PriorityOffset > 0 && Site + 7 <= End; ++Site)
        {
            const uint8_t ModRm = Byte(Site + 2);
            if (Byte(Site) == 0x0F && Byte(Site + 1) == 0x59 && (ModRm & 0xC0) == 0x80 && (ModRm & 0x07) != 0x04 && *reinterpret_cast<const int32_t*>(Site + 3) == PriorityOffset)
            {
                return true;
            }
        }
        return false;
    };

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
    const uintptr_t FoundPriority = NetPriorities == 1 ? VTableEntry(ActorVTable, NetPriority) : 0;
    if (ActorVTable && Velocity >= 0 && (!FoundPriority || !ScalesByPriority(FoundPriority, Finder::FunctionEnd(FoundPriority))))
    {
        // 1.10's GetNetPriority (0x1757820) is a leaf without unwind data, its call on the owner a tail jump, so the scan
        // above passes over it and meets another recursive function (slot 0x5E). Leaves are read 0x80 bytes in. 3.3
        // (0x1E70870, slot 0x71) has no other recursive function in the range, so the scan above finds none.
        NetPriorities = 0;
        for (int32_t Index = Velocity; Index < NetRelevant; ++Index)
        {
            const uintptr_t Function = VTableEntry(ActorVTable, Index);
            const uintptr_t FunctionEnd = Finder::FunctionEnd(Function);
            const uintptr_t End = FunctionEnd > Function && FunctionEnd - Function < 0x2000 ? FunctionEnd : Function + 0x80;
            bool bRecursive = false;
            for (uintptr_t Call = Function ? FindVirtualCall(Function, End) : 0; Call && !bRecursive; Call = FindVirtualCall(Call + 1, End))
            {
                bRecursive = VirtualSlot(Call) == Index;
            }
            if (bRecursive && ScalesByPriority(Function, End))
            {
                NetPriority = Index;
                ++NetPriorities;
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
    if (NetPriorities == 1 && NetDormancies != 1)
    {
        // UE 4.19 (1.10) drops the APlayerController* overload that forwarded into GetNetDormancy, so it follows
        // GetReplayPriority straight away: GetNetPriority 0x70, GetReplayPriority 0x71 (0x17591B0, "mulss xmm0,
        // [rcx+134h]"), GetNetDormancy 0x72. The slots after it hold shared stubs, which can't be told apart.
        const uintptr_t ReplayPriority = VTableEntry(ActorVTable, NetPriority + 1);
        NetDormancies = ScalesByPriority(ReplayPriority, ReplayPriority + 0x40) && VTableEntry(ActorVTable, NetPriority + 2) ? 1 : 0;
        NetDormancy = NetPriority + 2;
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
    int32_t ViewersNumOffset = ViewersData && (Byte(ViewersNum + 1) & 0xC0) == 0x80 ? *reinterpret_cast<const int32_t*>(ViewersNum + 2) : -1;
    int32_t ViewersDataOffset = ViewersData ? *reinterpret_cast<const int32_t*>(ViewersData + 4) : -1;
    if (!ViewersData && ReplicateActorEnd)
    {
        // 1.10 (0x19530C0) stores a register between GetWorldSettings' arguments and its call, then loads the count
        // ("movsxd rcx, [rax+500h]") and the data ("mov rbx, [rax+4F8h]") before scaling the count by 0x30.
        const uintptr_t SettingsArguments = FindInstruction(ReplicateActor, ReplicateActorEnd, { 0x41, 0xB0, 0x01, 0x33, 0xD2 });
        const uintptr_t SettingsCall = SettingsArguments ? FindRawCall(SettingsArguments, SettingsArguments + 0x20) : 0;
        const uintptr_t NumLoad = SettingsCall ? FindInstruction(SettingsCall + 5, SettingsCall + 0x30, { 0x48, 0x63, -1 }) : 0;
        const uintptr_t DataLoad = NumLoad && (Byte(NumLoad + 2) & 0xC7) == 0x80 ? FindInstruction(NumLoad + 7, NumLoad + 0x20, { 0x48, 0x8B, -1 }) : 0;
        const uintptr_t Stride = DataLoad && (Byte(DataLoad + 2) & 0xC7) == 0x80 ? FindInstruction(DataLoad + 7, DataLoad + 0x10, { 0x48, 0x8D, -1, -1, 0x48, 0xC1, -1, 0x04 }) : 0;
        ViewersNumOffset = Stride ? *reinterpret_cast<const int32_t*>(NumLoad + 3) : -1;
        ViewersDataOffset = Stride ? *reinterpret_cast<const int32_t*>(DataLoad + 3) : -1;
    }
    const UProperty* ViewersProperty = AWorldSettings::StaticClass() ? AWorldSettings::StaticClass()->FindPropertyByName(FName(L"ReplicationViewers", FNAME_Find)) : nullptr;
    const UScriptStruct* NetViewerStruct = FindObject<UScriptStruct>(nullptr, L"/Script/Engine.NetViewer");
    const int32_t ReflectedViewers = ViewersProperty ? ViewersProperty->GetOffset_ForInternal() : -1;
    const bool bViewersLayout = ViewersDataOffset == ReflectedViewers && ViewersNumOffset == ReflectedViewers + 8
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
    if (!ExecEnd && IsInText(Exec))
    {
        // A thunk that only forwards is a leaf without unwind data, so it has no known end (ACharacter::execClientCheatFly,
        // 1.7.2 0x25CEC50: Code++, then "jmp [rax+0x828]"): its tail jump is within its first 0x20 bytes.
        const uintptr_t Jump = FindVirtualCall(Exec, Exec + 0x20);
        return Jump && (Byte(Jump + 1) & 0x38) == 0x20 ? VirtualSlot(Jump) : -1;
    }
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
    uintptr_t EntryConstructor = EntrySetup ? Finder::RelativeTarget(EntrySetup + 13, 1, 5) : 0;
    const uintptr_t Next = EntrySetup ? FindRawCall(EntrySetup + 18, SpawnMissionItemPickupEnd) : 0;
    uintptr_t CreatePickup = Next ? Finder::RelativeTarget(Next, 1, 5) : 0;
    if (!EntrySetup && SpawnMissionItemPickupEnd)
    {
        // 1.10's constructor (0x6B0A90) takes no alterations, so nothing is stored for them before "lea r8d, [r9+1]"
        // (0x6FF37C); the entry still goes straight on (mov rdx, rax) to CreatePickup (0x6B6D10).
        const uintptr_t CountSetup = FindInstruction(SpawnMissionItemPickup, SpawnMissionItemPickupEnd, { 0x45, 0x8D, 0x41, 0x01, 0xE8 });
        // 4.1 stores the entry in its FFortCreatePickupData instead (mov [rsp+48h], rax, 0xDB623C) and passes that
        // (lea rcx, [rsp+40h]) to AFortPickup::CreateFromData (0xD73420), the next call.
        const bool bFromData = CountSetup && FindInstruction(CountSetup + 9, CountSetup + 14, { 0x48, 0x89, 0x44, 0x24, -1 }) == CountSetup + 9;
        const uintptr_t Pass = CountSetup ? (bFromData ? CountSetup + 9 : FindInstruction(CountSetup + 9, CountSetup + 0x30, { 0x48, 0x8B, 0xD0 })) : 0;
        const uintptr_t Create = Pass ? FindRawCall(Pass, SpawnMissionItemPickupEnd) : 0;
        EntryConstructor = Create ? Finder::RelativeTarget(CountSetup + 4, 1, 5) : 0;
        CreatePickup = Create ? Finder::RelativeTarget(Create, 1, 5) : 0;
        if (bFromData && CreatePickup)
        {
            Natives::AFortPickup_CreateFromData = reinterpret_cast<decltype(Natives::AFortPickup_CreateFromData)>(CreatePickup);
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

            // 1.10 keeps the authority check and Destroy of the give (0x6C77C0): "cmp byte [rcx+Role], 3; jnz; test rdx,
            // rdx; jz; mov r8b, 1; xor edx, edx; jmp AActor::Destroy; ret", with the inventory part stripped.
            const int32_t Slot = VirtualCalls >= 2 ? VirtualSlot(LastVirtualCall) : -1;
            const uintptr_t Give = VTableEntry(PickupVTable, Slot);
            // 4.1 (0xD821A0) reports the pickup to the Athena game state between the checks and Destroy, so it keeps
            // rbx for this: "push rbx; sub rsp, 20h; cmp byte [rcx+Role], 3; mov rbx, rcx; jnz; test rdx, rdx; jz;
            // ...; call; mov r8b, 1; xor edx, edx; mov rcx, rbx; add rsp, 20h; pop rbx; jmp AActor::Destroy".
            const auto IsReportingGive = [](uintptr_t Function)
            {
                if (FindInstruction(Function, Function + 23, { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x80, 0xB9, -1, -1, 0x00, 0x00, 0x03, 0x48, 0x8B, 0xD9, 0x75, -1, 0x48, 0x85, 0xD2, 0x74, -1 }) != Function)
                {
                    return false;
                }
                const uintptr_t Destroy = FindInstruction(Function + 23, Function + 0x50, { 0x41, 0xB0, 0x01, 0x33, 0xD2, 0x48, 0x8B, 0xCB, 0x48, 0x83, 0xC4, 0x20, 0x5B, 0xE9 });
                return Destroy && FindRawCall(Function + 23, Destroy) != 0;
            };
            if (Give && (reinterpret_cast<void*>(Give) == Natives::SharedEmptyFunction
                || FindInstruction(Give, Give + 25, { 0x80, 0xB9, -1, -1, 0x00, 0x00, 0x03, 0x75, -1, 0x48, 0x85, 0xD2, 0x74, -1, 0x41, 0xB0, 0x01, 0x33, 0xD2, 0xE9, -1, -1, -1, -1, 0xC3 }) == Give
                || IsReportingGive(Give)))
            {
                GiveIndex = Slot;
                Natives::AFortPickup_GivePickupToStripped = reinterpret_cast<void*>(Give);
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

    // The world manager's random stream is the third argument: lea r8, [reg+disp32] up to 4.5 (+0x938). From 5.0 the
    // world manager is loaded into r8 and the offset added in place, add r8, 940h (5.10 0xF88C20, 5.21 0xFAC2D0).
    ptrdiff_t StreamOffset = -1;
    for (uintptr_t Site = GenerateCall ? GenerateCall - 0x30 : 0; Site && Site + 7 <= GenerateCall; ++Site)
    {
        if ((Byte(Site) == 0x4C || Byte(Site) == 0x4D) && Byte(Site + 1) == 0x8D && (Byte(Site + 2) & 0xF8) == 0x80 && (Byte(Site + 2) & 0x07) != 0x04)
        {
            StreamOffset = *reinterpret_cast<const int32_t*>(Site + 3);
        }
        else if (Byte(Site) == 0x49 && Byte(Site + 1) == 0x81 && Byte(Site + 2) == 0xC0)
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
    // 5.21 ends a mission with UFortMcpProfileCampaign::EndMatch, for every player at once, and RemoveParticipantAccount
    // (0xF8DA40) no longer sends EndPrimaryMission or names the mission's status: it waits for the leaving player's
    // save and then takes them out of the participants (0xF79E00). Core sends EndMatch at zone end and names the status
    // through reflection (EFortMissionStatus), so neither is looked for there.
    const bool bHasEndMatch = FindObject<UFunction>(nullptr, L"/Script/FortniteGame.FortMcpProfileCampaign:EndMatch") != nullptr;
    if (Natives::AFortMissionManager_OnZoneEndIndex >= 0 && Natives::AFortMissionManager_OnMissionStatusChangeIndex >= 0 && Natives::AFortGameModeZone_OnZoneEndIndex >= 0 && Natives::AFortGameStateZone_SetGameplayStateIndex >= 0 && Natives::UFortQuestManager_SendStatEvent && Natives::UGameInstance_GetGameSessionId && (Natives::UEnum_GetEnumName || bHasEndMatch))
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

    // The thunk's first call past the parameter steps: 1.11's FUniqueNetIdRepl frees its ReplicationBytes after it.
    uintptr_t RemoveParticipant = 0;
    for (const auto& [Call, Target] : Calls(ExecOf(L"/Script/FortniteGame.FortMission:RemoveParticipantAccount")))
    {
        if (!RemoveParticipant && Target != Step && Target != StepExplicit)
        {
            RemoveParticipant = Target;
        }
    }
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
    // 1.10 strips UFortQuestManager::SendEndPrimaryMission: RemoveParticipantAccount (0x6FDCB0) calls the shared empty
    // function in its place, and Core sends the command itself (UFortQuestManager::SendEndPrimaryMission).
    // 2.5's (0xC87370) makes that call (0xC875E0) in a separate piece of the function (0xC87569 to 0xC875FE).
    std::vector<std::pair<uintptr_t, uintptr_t>> RemoveParticipantAllCalls = RemoveParticipantCalls;
    for (const auto& [ChunkBegin, ChunkEnd] : SendEndPrimaryMission ? std::vector<std::pair<uintptr_t, uintptr_t>>() : Finder::FunctionChunks(RemoveParticipant))
    {
        for (uintptr_t Call = ChunkBegin != RemoveParticipant ? FindRawCall(ChunkBegin, ChunkEnd) : 0; Call; Call = FindRawCall(Call + 1, ChunkEnd))
        {
            RemoveParticipantAllCalls.emplace_back(Call, Finder::RelativeTarget(Call, 1, 5));
        }
    }
    const bool bEndMissionStripped = !SendEndPrimaryMission && std::any_of(RemoveParticipantAllCalls.begin(), RemoveParticipantAllCalls.end(),
        [](const auto& Call) { return reinterpret_cast<void*>(Call.second) == Natives::SharedEmptyFunction; });
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

    const auto FirstEntryContaining = [](void** VTable, const std::vector<std::vector<int32_t>>& Patterns, const auto& ContainsFn, const auto& Accept)
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
            if (bAll && Accept(Function))
            {
                return Index;
            }
        }
        return -1;
    };
    const auto AnyEntry = [](uintptr_t) { return true; };

    // AFortGameModeZone::OnZoneEnd holds the zone's EndOfZoneRemainTime and ActiveSpawnPad offsets. 3.5 and 3.6 have an
    // inherited slot (0xC4, 3.6 0xC02E20) holding them before the zone end (0x15F, 3.5.2 0xC10710, 3.6 0xC18430), so the
    // slot taken is the first of them whose start calls AFortGameMode::SendStayWithParty, which the zone end does on
    // 3.3 to 4.0 and no other slot holding the offsets does. A build where none does takes the first of them, as before.
    void** ZoneVTable = CdoVTable(L"/Script/FortniteGame.FortGameModeZone");
    const std::vector<std::vector<int32_t>> ZoneOffsets{ Bytes32(EndOfZoneRemainTimeOffset), Bytes32(ActiveSpawnPadOffset) };
    const int32_t StaySlot = ClientStayWithPartySlot();
    int32_t GameModeIndex = StaySlot >= 0 ? FirstEntryContaining(ZoneVTable, ZoneOffsets, Contains, [StaySlot](uintptr_t Function) { return ZoneEndStayWithParty(Function, StaySlot) != 0; }) : -1;
    if (GameModeIndex < 0)
    {
        GameModeIndex = FirstEntryContaining(ZoneVTable, ZoneOffsets, Contains, AnyEntry);
    }

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
    int32_t GameStateIndex = OnRepIndex >= 0 ? FirstEntryContaining(GameStateVTable, { OnRepCall }, Contains, AnyEntry) : -1;
    if (OnRepIndex >= 0 && GameStateIndex < 0)
    {
        GameStateIndex = FirstEntryContaining(CdoVTable(L"/Script/FortniteGame.FortGameState"), { OnRepCall }, Contains, AnyEntry);
    }

    if (!SendStatEvent || (!SendEndPrimaryMission && !bEndMissionStripped && !bHasEndMatch) || !GameSessionId || (!GetEnumName && !bHasEndMatch) || MissionManagerIndex < 0 || StatusChangeIndex < 0 || GameModeIndex < 0 || GameStateIndex < 0)
    {
        Log::Error(L"Finder: zone end not found (stat {}, end mission {} (stripped {}, EndMatch {}, RemoveParticipantAccount 0x{:X}, {} calls), session {}, enum {}, mission manager {}, status change {}, game mode {}, game state {})", SendStatEvent != 0, SendEndPrimaryMission != 0,
            bEndMissionStripped, bHasEndMatch, RemoveParticipant ? RemoveParticipant - Finder::ImageBase() : 0, RemoveParticipantAllCalls.size(), GameSessionId != 0, GetEnumName != 0, MissionManagerIndex, StatusChangeIndex, GameModeIndex, GameStateIndex);
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
    if (bHasEndMatch && !SendEndPrimaryMission)
    {
        Log::Info(L"Finder: the build ends missions with UFortMcpProfileCampaign::EndMatch, Core sends EndMatch itself at zone end");
    }
    else if (bEndMissionStripped)
    {
        Log::Info(L"Finder: UFortQuestManager::SendEndPrimaryMission is stripped, Core sends EndPrimaryMission itself");
    }
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
    // Every frustrated AI asks (GAB_Hunting's SetCurrentGoalDiscouragement, BT_HuskBase's task): once the classes are
    // there, search and log once.
    static bool bSearched = false;
    if (bSearched)
    {
        return false;
    }

    // 5.0+ (UE 4.21): the test's RunTest calls the count of times its goal was discouraged itself (6.00 0xA863A0, 5.41
    // 0xA5F130, 5.30 0xA5C0F0), with the timers reader inlined: the assignment info lookup's result stays in a
    // callee-saved register and its DiscouragedGoalTimers are indexed from there (FAIDiscouragedGoalTimer is 0x28 bytes):
    //   call GetAssignmentInfo; mov rsi, rax; test rax, rax; ...; lea rcx, [rsi+68h]; ...; lea rcx, [rax+rax*4]
    const auto InlinedTimersReader = [](uintptr_t Target, uintptr_t TargetEnd, uintptr_t& OutLookup) -> int32_t
    {
        const uintptr_t First = TargetEnd > Target && TargetEnd - Target < 0x200 ? FindRawCall(Target, TargetEnd) : 0;
        const uintptr_t Save = First ? First + 5 : 0;
        if (!Save || (Byte(Save) != 0x48 && Byte(Save) != 0x4C) || Byte(Save + 1) != 0x8B || (Byte(Save + 2) & 0xC7) != 0xC0
            || !FindInstruction(Save, TargetEnd, { 0x48, 0x8D, 0x0C, 0x80 }))
        {
            return -1;
        }
        const int32_t Saved = ((Byte(Save + 2) >> 3) & 7) | (Byte(Save) == 0x4C ? 8 : 0);
        for (uintptr_t Site = Save + 3; Site + 4 <= TargetEnd; ++Site)
        {
            const uint8_t Rex = Byte(Site);
            const uint8_t ModRm = Byte(Site + 2);
            if ((Rex == 0x48 || Rex == 0x49) && Byte(Site + 1) == 0x8D && (ModRm & 0xF8) == 0x48 && (ModRm & 0x07) != 0x04
                && ((ModRm & 0x07) | (Rex == 0x49 ? 8 : 0)) == Saved)
            {
                OutLookup = Finder::RelativeTarget(First, 1, 5);
                return Byte(Site + 3);
            }
        }
        return -1;
    };

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
    // The timers sit in FAIAssignmentInfo's unreflected tail, after CurrentGoal (6.00: +0x68 of 0x80).
    const UScriptStruct* AssignmentInfo = FindObject<UScriptStruct>(nullptr, L"/Script/FortniteGame.AIAssignmentInfo");
    const int32_t InfoSize = AssignmentInfo ? AssignmentInfo->GetStructureSize() : 0;
    for (int32_t Index = 1; TestVTable && TestBaseVTable && Index < 0x80 && TimersOffset < 0; ++Index)
    {
        const uintptr_t Function = VTableEntry(TestVTable, Index);
        const uintptr_t FunctionEnd = Function && Function != VTableEntry(TestBaseVTable, Index) ? Finder::FunctionEnd(Function) : 0;
        for (const auto& [Call, Target] : FunctionEnd > Function ? Finder::DirectCalls(Function, FunctionEnd) : std::vector<std::pair<uintptr_t, uintptr_t>>{})
        {
            const uintptr_t TargetEnd = Finder::FunctionStart(Target) == Target ? Finder::FunctionEnd(Target) : 0;
            const int32_t Offset = InlinedTimersReader(Target, TargetEnd, Lookup);
            if (Offset >= 0x20 && Offset + 0x10 <= InfoSize)
            {
                TimersOffset = Offset;
                break;
            }
            Lookup = 0;
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
        bSearched = TestVTable && TaskVTable;
        Log::Error(L"Finder: goal discouragement not found (assignment info {}, timers {}, ExecuteTask slot {})", Lookup != 0, TimersOffset, ExecuteTaskIndex);
        return false;
    }
    Natives::AFortAIGoalManager_GetAssignmentInfo = reinterpret_cast<decltype(Natives::AFortAIGoalManager_GetAssignmentInfo)>(Lookup);
    Natives::FAIAssignmentInfo_DiscouragedGoalTimers = TimersOffset;
    Natives::UBTTaskNode_ExecuteTaskIndex = ExecuteTaskIndex;
    Log::Info(L"Finder: AFortAIGoalManager assignment info lookup at 0x{:X}, FAIAssignmentInfo discouraged goal timers +0x{:X}, UBTTaskNode::ExecuteTask slot 0x{:X}", Lookup - Finder::ImageBase(), TimersOffset, ExecuteTaskIndex);
    return true;
}

bool Finder::ResolveFrustrationDiscouragementLive(void** TaskVTable, ptrdiff_t DurationOffset)
{
    if (Natives::AFortAIGoalManager_SetFrustrationDiscouragement)
    {
        return true;
    }

    // ExecuteTask is the task's first slot that BTTaskNode's differs in, as ResolveGoalDiscouragementLive finds it.
    void** TaskNodeVTable = CdoVTable(L"/Script/AIModule.BTTaskNode");
    uintptr_t ExecuteTask = 0;
    for (int32_t Index = 1; TaskVTable && TaskNodeVTable && Index < 0x80 && !ExecuteTask; ++Index)
    {
        const uintptr_t Function = VTableEntry(TaskVTable, Index);
        ExecuteTask = Function && Function != VTableEntry(TaskNodeVTable, Index) ? Function : 0;
    }

    // 5.0+ (6.00 CL-4395664 0xAACE60, 5.41 0xA85A90, 5.30 0xA825C0) keeps Epic's body; only GetCurrent is stripped:
    //   AI = Cast<AFortAIController>(OwnerComp.AIOwner); GoalManager = AFortAIGoalManager::GetCurrent(AI->GetWorld());
    //   movss xmm2, [rdi+DiscouragementDuration]; mov rdx, AI; mov rcx, GoalManager; call SetFrustrationDiscouragement
    //   return succeeded ? EBTNodeResult::Succeeded : EBTNodeResult::Failed
    const uintptr_t End = ExecuteTask && DurationOffset > 0 && DurationOffset < 0x80 ? Finder::FunctionEnd(ExecuteTask) : 0;
    uintptr_t SetFrustration = 0;
    for (uintptr_t Site = ExecuteTask; End && Site + 6 <= End && !SetFrustration; ++Site)
    {
        const uintptr_t Opcode = Site + 1 + (Byte(Site + 1) >= 0x40 && Byte(Site + 1) <= 0x4F ? 1 : 0);
        const uint8_t ModRm = Byte(Opcode + 2);
        if (Byte(Site) == 0xF3 && Byte(Opcode) == 0x0F && Byte(Opcode + 1) == 0x10 && (ModRm & 0xF8) == 0x50 && (ModRm & 0x07) != 0x04 && Byte(Opcode + 3) == DurationOffset)
        {
            const uintptr_t Call = FindRawCall(Opcode + 4, std::min(End, Opcode + 4 + 0x10));
            SetFrustration = Call ? Finder::RelativeTarget(Call, 1, 5) : 0;
        }
    }
    if (!SetFrustration)
    {
        // 1.1-4.5: the task's ExecuteTask is stripped too, Core restores it.
        Log::Info(L"Finder: UFortBTTask_SetFrustrationDiscouragement::ExecuteTask is stripped (no DiscouragementDuration +0x{:X} call)", DurationOffset);
        return false;
    }
    Natives::AFortAIGoalManager_SetFrustrationDiscouragement = reinterpret_cast<decltype(Natives::AFortAIGoalManager_SetFrustrationDiscouragement)>(SetFrustration);
    Log::Info(L"Finder: AFortAIGoalManager::SetFrustrationDiscouragement at 0x{:X} (UFortBTTask_SetFrustrationDiscouragement::ExecuteTask 0x{:X} is the build's own)", SetFrustration - Finder::ImageBase(), ExecuteTask - Finder::ImageBase());
    return true;
}

bool Finder::ResolveQuickBarFocusLive()
{
    // The input binding names the action with a narrow string; the wide "SwitchQuickbar" copies are input-options text.
    // 3.0 binds it twice (pressed 0xE49090, released 0xE48EB0); the swap is in the released one, 2.5's only binding.
    std::vector<uintptr_t> Handlers;
    for (const uintptr_t Ref : Finder::StringRefs("SwitchQuickbar"))
    {
        uintptr_t Handler = 0;
        for (uintptr_t Site = Ref - 1; Site + 0x20 > Ref && !Handler; --Site)
        {
            if (Byte(Site) == 0x48 && Byte(Site + 1) == 0x8D && Byte(Site + 2) == 0x05)
            {
                Handler = Finder::RelativeTarget(Site, 3, 7);
            }
        }
        if (Handler && std::find(Handlers.begin(), Handlers.end(), Handler) == Handlers.end())
        {
            Handlers.push_back(Handler);
        }
    }

    // The handler's callee whose first three direct calls go to one function: AFortQuickBars::SwapQuickBarFocus, which
    // focuses through FocusSlot three times before anything else. (1.1 and 1.2 call the swap twice; 1.7.2 once.)
    uintptr_t Swap = 0;
    uintptr_t Focus = 0;
    for (const uintptr_t Handler : Handlers)
    {
        const uintptr_t HandlerEnd = IsInText(Handler) ? Finder::FunctionEnd(Handler) : 0;
        for (const auto& [Call, Target] : HandlerEnd > Handler ? Finder::DirectCalls(Handler, HandlerEnd) : std::vector<std::pair<uintptr_t, uintptr_t>>{})
        {
            const uintptr_t TargetEnd = Finder::FunctionStart(Target) == Target ? Finder::FunctionEnd(Target) : 0;
            const auto TargetCalls = TargetEnd > Target ? Finder::DirectCalls(Target, TargetEnd) : std::vector<std::pair<uintptr_t, uintptr_t>>{};
            // 3.0's swap (0xC77420) focuses through FocusSlot twice and then a virtual; its call passes (Primary, -1):
            // or r8d, -1; xor edx, edx, as 2.5's does.
            const bool bPrimaryNoOverride = Byte(Call - 6) == 0x41 && Byte(Call - 5) == 0x83 && Byte(Call - 4) == 0xC8 && Byte(Call - 3) == 0xFF && Byte(Call - 2) == 0x33 && Byte(Call - 1) == 0xD2;
            const bool bTwoFocuses = TargetCalls.size() >= 2 && TargetCalls[0].second == TargetCalls[1].second;
            // 3.6 (0xD15460) and 4.0 (0xD7B760) share the (Primary, -1) arguments with the other branch's (bar, slot)
            // through a jump, so the call has neither; the swap itself checks SlotOverride == -1 (cmp r8d, -1) before
            // its first FocusSlot.
            const bool bChecksSlotOverride = bTwoFocuses && FindInstruction(Target, TargetCalls[0].first, { 0x41, 0x83, 0xF8, 0xFF });
            if ((bTwoFocuses && TargetCalls.size() >= 3 && TargetCalls[1].second == TargetCalls[2].second) || (bTwoFocuses && bPrimaryNoOverride) || bChecksSlotOverride)
            {
                Swap = Target;
                Focus = TargetCalls[0].second;
                break;
            }
        }
        if (Swap)
        {
            break;
        }
    }
    if (!Swap || !Focus)
    {
        Log::Error(L"Finder: AFortQuickBars quickbar focus swap not found (handlers {}, swap {})", Handlers.size(), Swap != 0);
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

    // 5.21 (CL-4288479) saves more registers first: its lea rcx, [rdx+34h] is 0x24 bytes in (0xA73A54).
    int32_t OwnerOffset = -1;
    for (uintptr_t Site = Enemies; Site && Site + 4 <= Enemies + 0x40 && OwnerOffset < 0; ++Site)
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

bool Finder::ResolveAIPawnDeathLive(void** PawnVTable, ptrdiff_t UsesDBNOOffset, uint8_t UsesDBNOMask)
{
    if (Natives::AFortAIPawn_HandleDeathIndex >= 0)
    {
        return true;
    }

    // The encounter's handling of a pawn's death (4.0 0x8CF250, 4.1 0x8D46C0) is the one function with this log, and the
    // pawn's death notification (4.0 0xEEB6B0, 4.1 0xF01A90) its one caller. Two of AFortAIPawn's slots call that: its
    // Destroyed and its death handler (4.0 0xEEBAC0, slot 0x123), which tests bUsesDBNO (a knock-down goes elsewhere).
    const uintptr_t EncounterPawnDied = FunctionOf(L"Failed to find spawn group info on enemy pawn death for pawn %s. Associated Mission Name: %s. Group GUID is %s. Num active groups %i.");
    const std::vector<uintptr_t> EncounterPawnDiedCalls = EncounterPawnDied ? Finder::CallsTo(EncounterPawnDied, true) : std::vector<uintptr_t>();
    const uintptr_t NotifyDeath = EncounterPawnDiedCalls.size() == 1 ? Finder::FunctionStart(EncounterPawnDiedCalls.front()) : 0;

    // test byte ptr [reg+UsesDBNOOffset], UsesDBNOMask (F6 /0, disp32, imm8, no SIB).
    const auto TestsUsesDBNO = [UsesDBNOOffset, UsesDBNOMask](uintptr_t Function, uintptr_t End)
    {
        for (uintptr_t Site = Function; Site + 7 <= End; ++Site)
        {
            const uint8_t ModRm = Byte(Site + 1);
            if (Byte(Site) == 0xF6 && (ModRm & 0xF8) == 0x80 && (ModRm & 0x07) != 0x04 && *reinterpret_cast<const int32_t*>(Site + 2) == UsesDBNOOffset && Byte(Site + 6) == UsesDBNOMask)
            {
                return true;
            }
        }
        return false;
    };
    const int32_t Index = NotifyDeath && UsesDBNOOffset > 0 ? FirstVTableIndexWhere(PawnVTable, [&](uintptr_t Entry)
        {
            const uintptr_t Function = Finder::Unthunk(Entry);
            const uintptr_t End = Finder::FunctionStart(Function) == Function ? Finder::FunctionEnd(Function) : 0;
            if (!End || !TestsUsesDBNO(Function, End))
            {
                return false;
            }
            for (const auto& [Call, Target] : Finder::DirectCalls(Function, End))
            {
                if (Finder::Unthunk(Target) == NotifyDeath)
                {
                    return true;
                }
            }
            return false;
        }) : -1;

    if (Index < 0)
    {
        Log::Error(L"Finder: AFortAIPawn's death handler not found (encounter 0x{:X}, {} calls, notify 0x{:X})", EncounterPawnDied ? EncounterPawnDied - Finder::ImageBase() : 0, EncounterPawnDiedCalls.size(), NotifyDeath ? NotifyDeath - Finder::ImageBase() : 0);
        return false;
    }
    Natives::AFortAIPawn_HandleDeathIndex = Index;
    Log::Info(L"Finder: AFortAIPawn's death handler is slot 0x{:X} (0x{:X})", Index, reinterpret_cast<uintptr_t>(PawnVTable[Index]) - Finder::ImageBase());
    return true;
}

bool Finder::ResolveAssignmentGoalChecksLive(ptrdiff_t MaxAIForAssignmentOffset, ptrdiff_t MaxAIPerGoalOffset)
{
    if (Natives::UFortAIAssignment_CanPawnUse && Natives::FFortAIGoal_CanAIUse)
    {
        return true;
    }

    // Whether a function loads a float from [reg+Offset] with movss (F3 [REX] 0F 10, disp32, no SIB).
    const auto LoadsFloatAt = [](uintptr_t Function, ptrdiff_t Offset)
    {
        const uintptr_t End = Offset > 0 && Finder::FunctionStart(Function) == Function ? Finder::FunctionEnd(Function) : 0;
        for (uintptr_t Site = Function; End && Site + 8 <= End; ++Site)
        {
            const uintptr_t Opcode = Site + 1 + (Byte(Site + 1) >= 0x40 && Byte(Site + 1) <= 0x4F ? 1 : 0);
            const uint8_t ModRm = Byte(Opcode + 2);
            if (Byte(Site) == 0xF3 && Byte(Opcode) == 0x0F && Byte(Opcode + 1) == 0x10 && (ModRm & 0xC0) == 0x80 && (ModRm & 0x07) != 0x04 && *reinterpret_cast<const int32_t*>(Opcode + 3) == Offset)
            {
                return true;
            }
        }
        return false;
    };

    // The goal loop (1.9 0x315C20) rejects a query result's assignment through its pawn check (1.9 0x2D5C80, reads
    // MaxAIAllowedForAssignment) and the goal through its AI check (1.9 0x3679B0, reads MaxAIAllowedPerGoal).
    const uintptr_t Loop = FunctionOf(L"Found only invalid goal(s).  Pawn: %s");
    const uintptr_t LoopEnd = Loop ? Finder::FunctionEnd(Loop) : 0;
    uintptr_t PawnCheck = 0;
    uintptr_t GoalCheck = 0;
    for (const auto& [Call, Target] : LoopEnd > Loop ? Finder::DirectCalls(Loop, LoopEnd) : std::vector<std::pair<uintptr_t, uintptr_t>>{})
    {
        if (!PawnCheck && LoadsFloatAt(Target, MaxAIForAssignmentOffset))
        {
            PawnCheck = Target;
        }
        else if (!GoalCheck && Target != PawnCheck && LoadsFloatAt(Target, MaxAIPerGoalOffset))
        {
            GoalCheck = Target;
        }
    }

    if (!PawnCheck || !GoalCheck)
    {
        Log::Error(L"Finder: assignment goal checks not found (goal loop {}, pawn check {}, goal check {})", Loop != 0, PawnCheck != 0, GoalCheck != 0);
        return false;
    }
    Natives::UFortAIAssignment_CanPawnUse = reinterpret_cast<decltype(Natives::UFortAIAssignment_CanPawnUse)>(PawnCheck);
    Natives::FFortAIGoal_CanAIUse = reinterpret_cast<decltype(Natives::FFortAIGoal_CanAIUse)>(GoalCheck);
    Log::Info(L"Finder: assignment pawn check at 0x{:X}, goal AI check at 0x{:X}", PawnCheck - Finder::ImageBase(), GoalCheck - Finder::ImageBase());
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
        uintptr_t CachedType = TypeGetter ? FindInstruction(TypeGetter, TypeGetter + 0x10, { 0x48, 0x83, 0x3D, -1, -1, -1, -1, 0x00 }) : 0;
        uintptr_t CachedTypeSlot = CachedType ? Finder::RelativeTarget(CachedType, 3, 8) : 0;
        if (TypeGetter && !CachedType)
        {
            // 1.10's StaticClass (0xADFC40) loads the cached class instead: "mov rax, [rip+X]; test rax, rax".
            CachedType = FindInstruction(TypeGetter, TypeGetter + 0x18, { 0x48, 0x8B, 0x05, -1, -1, -1, -1, 0x48, 0x85, 0xC0 });
            CachedTypeSlot = CachedType ? Finder::RelativeTarget(CachedType, 3, 7) : 0;
        }
        if (CachedType && GoalItemType && *reinterpret_cast<UClass* const*>(CachedTypeSlot) == GoalItemType)
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

bool Finder::ResolveGameSessionVerifyAuthLive(void** SessionVTable)
{
    // AFortGameSession::OnVerifyAuthComplete (2.4 0x70A130, 2.3 0x70D630) logs this before it kicks the player whose
    // UniqueId matches AuthToken.AccountId; it is the slot whose code runs into that log (2.3 and 2.4 slot 0xDC).
    const uintptr_t KickRef = First(Finder::StringRefs(L"User auth check failed. Server requesting to kick user to login screen UserId=%s bSuccess=%d Error=%s"));
    int32_t VerifyIndex = -1;
    uintptr_t Verify = 0;
    for (int32_t Index = 0; KickRef && Index < 0x400 && VerifyIndex < 0; ++Index)
    {
        const uintptr_t Entry = VTableEntry(SessionVTable, Index);
        if (Entry && Entry <= KickRef && KickRef - Entry < 0x1000 && Finder::RunsInto(Entry, KickRef, 512) >= 0)
        {
            VerifyIndex = Index;
            Verify = Entry;
        }
    }
    if (VerifyIndex < 0)
    {
        Log::Warning(L"Finder: AFortGameSession::OnVerifyAuthComplete not found (kick log {})", KickRef != 0);
        return false;
    }

    // Up to 2.3 its first branch is `if (!bWasSuccessful)`: test dl, dl; jnz return (2.3 0x70D630). 2.4 opens with the
    // AuthToken.TokenType test instead (cmp [r8+0F4h], r14d; jz) and only logs bWasSuccessful.
    uintptr_t Previous = 0;
    const uintptr_t Branch = Finder::TraceFind(Verify, 32, [&](uintptr_t Instruction)
    {
        const uint8_t Op = Byte(Instruction);
        const bool bConditional = (Op >= 0x70 && Op <= 0x7F) || (Op == 0x0F && Byte(Instruction + 1) >= 0x80 && Byte(Instruction + 1) <= 0x8F);
        if (!bConditional)
        {
            Previous = Instruction;
        }
        return bConditional;
    });
    const bool bTestsSuccess = !Branch || (Previous && Byte(Previous) == 0x84 && Byte(Previous + 1) == 0xD2);

    Natives::AFortGameSession_OnVerifyAuthCompleteIndex = VerifyIndex;
    Natives::AFortGameSession_bOnVerifyAuthCompleteTestsSuccess = bTestsSuccess;
    Log::Info(L"Finder: AFortGameSession::OnVerifyAuthComplete is slot 0x{:X} (0x{:X}), {}", VerifyIndex, Verify - Finder::ImageBase(),
        bTestsSuccess ? L"tests bWasSuccessful" : L"kicks whatever bWasSuccessful is");
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

    // With an inventory already bound (a player rejoining the zone) the bind hands it to the controller through
    // ReconnectOutpostInventory or ReconnectWorldInventory, with GetOutpostInventory (which the bind's first-bind branch
    // calls) between them in the vtable. 1.7.2-1.10 tail-call the two (`jmp [rax+disp32]`, 1.10 0x784514 / 0x78452A:
    // +0x15E8 / +0x15D8; 1.9.1 +0x1600 / +0x15F0). 1.11+ call them (`call [rax+disp32]`) and then clear the inventory's
    // bAllowMcpItemCreation (3.0 0xD1C76C / 0xD1C774: +0x1660 / +0x1650, GetOutpostInventory +0x1658; 2.2 +0x1610 /
    // +0x1600; 1.11 +0x1600 / +0x15F0). Collect every vtable call and jump in the bind and take the three adjacent slots.
    std::vector<int32_t> VirtualCallDisps;
    for (uintptr_t Site = Bind; Site && Site + 7 <= BindEnd; ++Site)
    {
        if (Byte(Site) == 0xFF && Byte(Site + 1) == 0x90 && Byte(Site + 4) == 0x00 && Byte(Site + 5) == 0x00)
        {
            VirtualCallDisps.push_back(*reinterpret_cast<const int32_t*>(Site + 2));
        }
        else if (Byte(Site) == 0x48 && Byte(Site + 1) == 0xFF && Byte(Site + 2) == 0xA0 && Byte(Site + 5) == 0x00 && Byte(Site + 6) == 0x00)
        {
            VirtualCallDisps.push_back(*reinterpret_cast<const int32_t*>(Site + 3));
        }
    }
    const auto HasVirtualCall = [&VirtualCallDisps](int32_t Disp)
    {
        return std::find(VirtualCallDisps.begin(), VirtualCallDisps.end(), Disp) != VirtualCallDisps.end();
    };
    int32_t GetOutpostInventoryDisp = 0;
    for (const int32_t Disp : VirtualCallDisps)
    {
        if (!GetOutpostInventoryDisp && Disp >= 0x1000 && HasVirtualCall(Disp - 8) && HasVirtualCall(Disp + 8))
        {
            GetOutpostInventoryDisp = Disp;
        }
    }
    if (GetOutpostInventoryDisp)
    {
        Natives::AFortPlayerController_ReconnectWorldInventoryIndex = (GetOutpostInventoryDisp - 8) / 8;
        Natives::AFortPlayerController_ReconnectOutpostInventoryIndex = (GetOutpostInventoryDisp + 8) / 8;
        Log::Info(L"Finder: AFortPlayerController::ReconnectWorldInventory is slot 0x{:X}, ReconnectOutpostInventory slot 0x{:X}", Natives::AFortPlayerController_ReconnectWorldInventoryIndex, Natives::AFortPlayerController_ReconnectOutpostInventoryIndex);
    }
    else
    {
        Log::Info(L"Finder: UFortMcpProfileWorld::BindToPlayerController hands no existing inventory to the controller in this build");
    }
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
        if (Finder::BodyIs(Callee, { 0x33, 0xC0, 0xC3 }) && Call >= 6 && (Byte(Call - 6) == 0xFF || Byte(Call - 9) == 0xFF))
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

    // From 5.0 (UE 4.21) CanCraftSchematic inlines GetSchematicQuantityProduced, so no call to it follows
    // GetSchematicForItem. A missing ingredient's schematic is then (5.0 CL-4204761 0xF347F2, 6.02 CL-4461277 0xFA5FE2)
    // "call GetSchematicForItem; mov r12, rax; test rax, rax; jz; mov rdx, [rax]; mov rcx, rax; call [rdx+398h]": the
    // GetItemDefinitionBP call GetSchematicQuantityProduced opens with, made on the schematic just found. The rest of the
    // inlined body follows, and then CanCraftSchematic calls itself for that schematic. So GetSchematicForItem is the
    // direct call right before the last such virtual call ahead of the recursion.
    // The rule this replaces took the last direct call before the recursion that GetSchematicQuantityProduced doesn't
    // make itself. That was never GetSchematicForItem: 5.0 to 5.40 and 6.01 leave GetSchematicQuantityProduced through a
    // jmp, which isn't a call, so its target was taken (5.0 0xF43A70, the quantity read); 6.00 and 6.02 inline that too,
    // the function outgrew the size the rule read it up to, and the last call left was FMemory::Free (6.02 0x161BB70),
    // called on the player controller when a craft was short of an ingredient that has its own schematic.
    if (CanCraftEnd && !GetSchematicForItem)
    {
        const uintptr_t QuantityProducedEnd = Finder::FunctionEnd(QuantityProduced);
        const int32_t GetItemDefinitionIndex = QuantityProducedEnd ? VirtualSlot(FindVirtualCall(QuantityProduced, QuantityProducedEnd)) : -1;

        uintptr_t Recursion = 0;
        for (const auto& [Site, Target] : Finder::DirectCalls(CanCraft, CanCraftEnd))
        {
            if (Target == CanCraft)
            {
                Recursion = Site;
                break;
            }
        }

        uintptr_t IngredientSchematicDefinition = 0;
        for (uintptr_t Site = Recursion && GetItemDefinitionIndex >= 0 ? FindVirtualCall(CanCraft, Recursion) : 0; Site; Site = FindVirtualCall(Site + 1, Recursion))
        {
            if (VirtualSlot(Site) == GetItemDefinitionIndex)
            {
                IngredientSchematicDefinition = Site;
            }
        }

        // The call's result is tested and becomes the virtual call's object: 23 bytes apart in 5.0 to 6.02.
        constexpr uintptr_t MaxBytesToVirtualCall = 0x30;
        for (const auto& [Site, Target] : IngredientSchematicDefinition ? Finder::DirectCalls(CanCraft, IngredientSchematicDefinition) : std::vector<std::pair<uintptr_t, uintptr_t>>())
        {
            GetSchematicForItem = Site + MaxBytesToVirtualCall >= IngredientSchematicDefinition && Target != CanCraft && Target != QuantityProduced ? Target : 0;
        }
    }

    if (CraftIndex < 0 || !CanCraft || !GetSchematicForItem)
    {
        Log::Error(L"Finder: schematic crafting not found");
        return false;
    }
    Natives::UFortSchematicItem_ServerCraftSchematicIndex = CraftIndex;
    Natives::UFortSchematicItem_CanCraftSchematic = reinterpret_cast<decltype(Natives::UFortSchematicItem_CanCraftSchematic)>(CanCraft);
    Natives::AFortPlayerController_GetSchematicForItem = reinterpret_cast<decltype(Natives::AFortPlayerController_GetSchematicForItem)>(GetSchematicForItem);
    Log::Info(L"Finder: UFortSchematicItem::ServerCraftSchematic is slot 0x{:X} (CanCraftSchematic 0x{:X}, AFortPlayerController::GetSchematicForItem 0x{:X})",
        CraftIndex, CanCraft - Finder::ImageBase(), GetSchematicForItem - Finder::ImageBase());
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

// The function that gives a mission actor its mission event params: every object property typed as a subclass of
// UFortMissionEventParams that is still null gets a NewObject of that class (a blueprint's DepositStart_ObjParam,
// GenDeposit, ...), which it then fills with SetParams and sends with ExecuteMissionEventEx. AFortMissionState's
// PostInitializeComponents calls it right after Super (2.5 0xC85C40 -> 0xC720E0, 2.4.2 0x706F80 -> 0x6F3B70); it is
// the call there whose callee loads the UFortMissionEventParams class (its StaticClass, "mov rax, [rip+X]; test rax,
// rax"). That slot is AActor::PostInitializeComponents. 2.2 to 2.4.2's ABuildingActor::PostInitializeComponents calls it
// too, for buildings that implement UFortMissionInterface (2.4.2 0x472580); 2.5's (0x9F69F0) does not.
//
// 5.30 to 6.01 inline it into AFortMissionState::PostInitializeComponents (6.00 CL-4395664 0x10145B0, 5.41 0xFD4140:
// Super, then the loop over the class's object properties, which calls UFortMissionEventParams::StaticClass and
// StaticConstructObject_Internal itself), so there is no function to call. The override that does all three gives the
// slot, Natives::InitializeMissionEventParams stays null, and the caller runs its own copy of the loop.
bool Finder::ResolveMissionEventParamsLive()
{
    if (Natives::AActor_PostInitializeComponentsIndex >= 0)
    {
        return true;
    }

    UClass* ParamsClass = FindObject<UClass>(nullptr, L"/Script/FortniteGame.FortMissionEventParams");
    const auto LoadsParamsClass = [ParamsClass](uintptr_t Function)
    {
        const uintptr_t End = IsInText(Function) ? Finder::FunctionEnd(Function) : 0;
        for (const auto& [Site, Target] : End && End - Function < 0x400 ? Finder::DirectCalls(Function, End) : std::vector<std::pair<uintptr_t, uintptr_t>>())
        {
            // Before 1.10 a StaticClass compares the cached class instead: "cmp qword ptr [rip+X], 0".
            const uintptr_t Load = FindInstruction(Target, Target + 0x18, { 0x48, 0x8B, 0x05, -1, -1, -1, -1, 0x48, 0x85, 0xC0 });
            const uintptr_t Compare = Load ? 0 : FindInstruction(Target, Target + 0x18, { 0x48, 0x83, 0x3D, -1, -1, -1, -1, 0x00 });
            const uintptr_t Cached = Load ? Finder::RelativeTarget(Load, 3, 7) : Compare ? Finder::RelativeTarget(Compare, 3, 8) : 0;
            if (Cached && *reinterpret_cast<UClass* const*>(Cached) == ParamsClass)
            {
                return true;
            }
        }
        return false;
    };

    void** ActorVTable = CdoVTable(L"/Script/Engine.Actor");
    void** MissionStateVTable = CdoVTable(L"/Script/FortniteGame.FortMissionState");
    int32_t PostInitializeIndex = -1;
    uintptr_t Initialize = 0;
    int32_t Matches = 0;
    for (int32_t Index = 0; ParamsClass && ActorVTable && MissionStateVTable && Index < 0x200; ++Index)
    {
        const uintptr_t Entry = VTableEntry(MissionStateVTable, Index);
        const uintptr_t Super = VTableEntry(ActorVTable, Index);
        if (!Entry || !Super)
        {
            break;
        }
        const uintptr_t EntryEnd = Entry != Super ? Finder::FunctionEnd(Entry) : 0;
        if (!EntryEnd || EntryEnd - Entry > 0x200)
        {
            continue;
        }

        const std::vector<std::pair<uintptr_t, uintptr_t>> Calls = Finder::DirectCalls(Entry, EntryEnd);
        const bool bCallsSuper = std::any_of(Calls.begin(), Calls.end(), [Super](const auto& Call) { return Call.second == Super; });
        for (const auto& [Site, Target] : bCallsSuper ? Calls : std::vector<std::pair<uintptr_t, uintptr_t>>())
        {
            if (Target != Super && LoadsParamsClass(Target))
            {
                PostInitializeIndex = Index;
                Initialize = Target;
                ++Matches;
            }
        }
    }

    // The inlined form, only where no build function was found: the override that calls Super, loads the
    // UFortMissionEventParams class itself and constructs the objects itself.
    int32_t InlinedMatches = 0;
    const uintptr_t Construct = reinterpret_cast<uintptr_t>(Natives::StaticConstructObject_Internal);
    for (int32_t Index = 0; Matches == 0 && Construct && ParamsClass && ActorVTable && MissionStateVTable && Index < 0x200; ++Index)
    {
        const uintptr_t Entry = VTableEntry(MissionStateVTable, Index);
        const uintptr_t Super = VTableEntry(ActorVTable, Index);
        if (!Entry || !Super)
        {
            break;
        }
        const uintptr_t EntryEnd = Entry != Super ? Finder::FunctionEnd(Entry) : 0;
        if (!EntryEnd || EntryEnd - Entry >= 0x400)
        {
            continue;
        }

        const std::vector<std::pair<uintptr_t, uintptr_t>> Calls = Finder::DirectCalls(Entry, EntryEnd);
        const bool bCallsSuper = std::any_of(Calls.begin(), Calls.end(), [Super](const auto& Call) { return Call.second == Super; });
        const bool bConstructs = std::any_of(Calls.begin(), Calls.end(), [Construct](const auto& Call) { return Call.second == Construct; });
        if (bCallsSuper && bConstructs && LoadsParamsClass(Entry))
        {
            PostInitializeIndex = Index;
            ++InlinedMatches;
        }
    }

    if (Matches != 1 && InlinedMatches != 1)
    {
        Log::Error(L"Finder: mission event params initialization not found ({} matches, {} inlined)", Matches, InlinedMatches);
        return false;
    }
    Natives::AActor_PostInitializeComponentsIndex = PostInitializeIndex;
    Natives::InitializeMissionEventParams = reinterpret_cast<decltype(Natives::InitializeMissionEventParams)>(Initialize);
    if (Initialize)
    {
        Log::Info(L"Finder: mission event params initialization at +0x{:X}, AActor::PostInitializeComponents slot 0x{:X}", Initialize - Finder::ImageBase(), PostInitializeIndex);
    }
    else
    {
        Log::Info(L"Finder: mission event params initialization is inlined into AFortMissionState::PostInitializeComponents (+0x{:X}), AActor::PostInitializeComponents slot 0x{:X}",
            VTableEntry(MissionStateVTable, PostInitializeIndex) - Finder::ImageBase(), PostInitializeIndex);
    }
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
// passes the mission manager's EarnedBadgesArray in rcx. 1.11's objective blocks reach a shared mission points grant
// (0x713B20) that passes it too, from three slots, so a callee's callee only one slot reaches decides it.
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
    std::vector<std::pair<int32_t, uintptr_t>> Passing;
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
        for (const auto& [Site, Target] : EntryEnd && EntryEnd - Entry < 0x1000 ? Finder::DirectCalls(Entry, EntryEnd) : std::vector<std::pair<uintptr_t, uintptr_t>>())
        {
            const uintptr_t TargetEnd = IsInText(Target) ? Finder::FunctionEnd(Target) : 0;
            for (const auto& [InnerSite, Inner] : TargetEnd && TargetEnd - Target < 0x1000 ? Finder::DirectCalls(Target, TargetEnd) : std::vector<std::pair<uintptr_t, uintptr_t>>())
            {
                const bool bKnown = std::find(Passing.begin(), Passing.end(), std::pair{ Index, Inner }) != Passing.end();
                if (!bKnown && IsInText(Inner) && PassesBadgeArray(Inner))
                {
                    Passing.emplace_back(Index, Inner);
                }
            }
        }
    }

    int32_t BadgeIndex = -1;
    int32_t Matches = 0;
    for (const auto& [Index, Inner] : Passing)
    {
        const auto SameInner = std::count_if(Passing.begin(), Passing.end(), [Inner](const auto& Other) { return Other.second == Inner; });
        if (SameInner == 1 && Index != BadgeIndex)
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
    if (Natives::UFortMcpProfileCampaign_GetModeLoadout && Natives::UFortRegisteredPlayerInfo_GetAbilitySystemInterface && Natives::UFortHomebaseManager_GetSelectableGadgetItems && Natives::UFortHomebaseManager_GetOwnedNodes)
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
        // 5.0 inlines GetOwnedNodeAbilityKits: GetSelectableGadgetItems calls GetOwnedNodes itself (5.0 +0x86, past
        // an AbilitySystem->GetAbilitySystemComponent() call). Core then builds the kits from GetOwnedNodes.
        else if (IsInText(Target) && CallWithin(Target, 0xC0, OwnedNodes))
        {
            AbilitySystemInterface = Previous;
            GadgetItems = Target;
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
    Natives::UFortMcpProfileCampaign_GetModeLoadout = reinterpret_cast<decltype(Natives::UFortMcpProfileCampaign_GetModeLoadout)>(ModeLoadout);
    Natives::UFortRegisteredPlayerInfo_GetAbilitySystemInterface = reinterpret_cast<decltype(Natives::UFortRegisteredPlayerInfo_GetAbilitySystemInterface)>(AbilitySystemInterface);
    Natives::UFortHomebaseManager_GetSelectableGadgetItems = reinterpret_cast<decltype(Natives::UFortHomebaseManager_GetSelectableGadgetItems)>(GadgetItems);
    Natives::UFortHomebaseManager_GetOwnedNodeAbilityKits = reinterpret_cast<decltype(Natives::UFortHomebaseManager_GetOwnedNodeAbilityKits)>(OwnedNodeKits);
    Natives::UFortHomebaseManager_GetOwnedNodes = reinterpret_cast<decltype(Natives::UFortHomebaseManager_GetOwnedNodes)>(OwnedNodes);
    Log::Info(L"Finder: homebase gadgets found{}", OwnedNodeKits ? L"" : L" (GetOwnedNodeAbilityKits inlined)");
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
    int32_t GoalManagerIndex = -1;
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
                GoalManagerIndex = Slot;
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
    Natives::AFortGameModeZone_CreateAIGoalManagerIndex = GoalManagerIndex;
    Log::Info(L"Finder: AFortGameModeZone::CreateAIDirector is slot 0x{:X}", DirectorIndex);
    return true;
}

uint8_t* Finder::AthenaCreateAIDirectorSuperCall(ptrdiff_t AISettingsOffset)
{
    // 6.20 (CL-4497486) gives Battle Royale an AI of its own for the Fortnitemares Cube Monsters (the Deimos playlists'
    // AISettings, Athena_AI_Settings). AFortGameModeAthena overrides CreateAIDirector (0xC67A90, slot 0x180): when the
    // game mode's AISettings allow an AI director ("test byte ptr [rax+30h], 1", bAllowAIDirector), it calls
    // AFortGameModeZone::CreateAIDirector directly ("call 0x2A96C0" at 0xC67AAB, the stripped function folded into the
    // shared empty one), then sets the director's MaxActiveAlive from the game mode's MaxActiveAIActors ("setting
    // initial MaxActiveAlive override"). The zone's slot is swapped to Core's restoration; this direct call can't be,
    // so it is the call that is redirected. Builds whose Athena game mode has no AISettings (before 5.0), or no
    // CreateAIDirector of its own, have none.
    const int32_t DirectorIndex = Natives::AFortGameModeZone_CreateAIDirectorIndex;
    void** AthenaVTable = DirectorIndex >= 0 && AISettingsOffset > 0 ? CdoVTable(L"/Script/FortniteGame.FortGameModeAthena") : nullptr;
    const uintptr_t Own = AthenaVTable ? VTableEntry(AthenaVTable, DirectorIndex) : 0;
    if (!Own || reinterpret_cast<void*>(Own) == Natives::SharedEmptyFunction || IsCoreFunction(Own) || !IsInText(Own))
    {
        return nullptr;
    }
    if (!Finder::ReadsMemberAt(Own, static_cast<int32_t>(AISettingsOffset)))
    {
        Log::Info(L"Finder: AFortGameModeAthena's CreateAIDirector (0x{:X}) doesn't read its AISettings (+0x{:X}), so it isn't 6.20's, and calls no stripped AFortGameModeZone::CreateAIDirector",
            Own - Finder::ImageBase(), AISettingsOffset);
        return nullptr;
    }
    std::vector<uintptr_t> StubCalls;
    for (const auto& [Site, Target] : Finder::DecodedDirectCalls(Own))
    {
        if (reinterpret_cast<void*>(Target) == Natives::SharedEmptyFunction)
        {
            StubCalls.push_back(Site);
        }
    }
    if (StubCalls.size() != 1)
    {
        Log::Error(L"Finder: AFortGameModeAthena::CreateAIDirector (0x{:X}) makes {} calls to the shared empty function, not the one to the stripped AFortGameModeZone::CreateAIDirector, "
            L"so Battle Royale's AI playlists get no AI director", Own - Finder::ImageBase(), StubCalls.size());
        return nullptr;
    }
    Log::Info(L"Finder: AFortGameModeAthena::CreateAIDirector (0x{:X}) calls the stripped AFortGameModeZone::CreateAIDirector at 0x{:X}", Own - Finder::ImageBase(), StubCalls[0] - Finder::ImageBase());
    return reinterpret_cast<uint8_t*>(StubCalls[0]);
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
    // 4.2's is /Script/NavigationSystem.NavigationSystemV1.
    UClass* NavigationSystemClass = UNavigationSystem::StaticClass();
    UObject* NavigationSystemDefault = NavigationSystemClass ? NavigationSystemClass->GetDefaultObject() : nullptr;
    void** NavigationSystemVTable = NavigationSystemDefault ? *reinterpret_cast<void***>(NavigationSystemDefault) : nullptr;
    uintptr_t Build = VTableEntry(NavigationSystemVTable, BuildSlot);
    if (!Build && CheatSlot < 0)
    {
        // 4.2 drops UCheatManager::RebuildNavigation; UNavigationSystemV1::Build (0x26F1A20) logs its time as it ends.
        Build = FunctionOf(L"UNavigationSystemV1::Build total execution time: %.5f");
        BuildSlot = Build ? Finder::VTableIndex(NavigationSystemVTable, reinterpret_cast<void*>(Build), 0x400) : -1;
        Build = BuildSlot >= 0 ? Build : 0;
    }
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
    // 4.2's UNavigationSystemV1 makes ProcessRegistrationCandidates virtual (0x26F1AE5, call [rax+2A8h] right after
    // SpawnMissingNavigationData's call [rax+2F8h]), so the first raw call after them is RebuildAll there.
    const uintptr_t VirtualCandidates = SpawnMissing ? FindVirtualCall(SpawnMissing + 6, SpawnMissing + 0x18) : 0;
    const int32_t ProcessCandidatesSlot = VirtualCandidates && !FindRawCall(SpawnMissing + 6, VirtualCandidates) ? VirtualSlot(VirtualCandidates) : -1;
    const uintptr_t ProcessCandidates = ProcessCandidatesSlot < 0 && SpawnMissing ? FindRawCall(SpawnMissing, BuildEnd) : 0;
    const uintptr_t RebuildAllCall = ProcessCandidatesSlot >= 0 ? FindRawCall(VirtualCandidates + 6, BuildEnd) : ProcessCandidates ? FindRawCall(ProcessCandidates + 5, BuildEnd) : 0;
    const uintptr_t RebuildAll = RebuildAllCall ? Finder::RelativeTarget(RebuildAllCall, 1, 5) : 0;

    const int32_t SpawnMissingSlot = VirtualSlot(SpawnMissing);
    const uintptr_t ProcessCandidatesTarget = ProcessCandidates ? Finder::RelativeTarget(ProcessCandidates, 1, 5) : 0;
    uintptr_t PopulateNavOctree = 0;
    int32_t PopulateCandidates = 0;
    // The world's init (4.2 0x271E6D1) runs the same pair, then ConditionalPopulateNavOctree.
    for (uintptr_t Call = ProcessCandidatesSlot >= 0 && SpawnMissingSlot >= 0 ? FindVirtualCall(Finder::TextBegin(), Finder::TextEnd()) : 0; Call; Call = FindVirtualCall(Call + 1, Finder::TextEnd()))
    {
        if (VirtualSlot(Call) != SpawnMissingSlot || Call == SpawnMissing)
        {
            continue;
        }
        const uintptr_t Candidates = FindVirtualCall(Call + 6, Call + 0x18);
        if (!Candidates || VirtualSlot(Candidates) != ProcessCandidatesSlot || FindRawCall(Call + 6, Candidates))
        {
            continue;
        }
        const uintptr_t Next = FindRawCall(Candidates + 6, Candidates + 18);
        const uintptr_t Target = Next ? Finder::RelativeTarget(Next, 1, 5) : 0;
        if (Target && Target != RebuildAll && Target != PopulateNavOctree)
        {
            PopulateNavOctree = Target;
            ++PopulateCandidates;
        }
    }
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
    for (uintptr_t Site = PopulateNavOctree; PopulateEnd && Site + 9 <= PopulateEnd && OctreeLockOffset < 0; ++Site)
    {
        // 4.2 (0x26F6F9C) tests it through r12: test byte [r12+39Ah], 1 (41 F6 84 24 disp32 01).
        if (Byte(Site) == 0x41 && Byte(Site + 1) == 0xF6 && Byte(Site + 2) == 0x84 && Byte(Site + 3) == 0x24 && Byte(Site + 8) == 0x01)
        {
            OctreeLockOffset = *reinterpret_cast<const int32_t*>(Site + 4);
        }
    }
    if (PopulateEnd && OctreeLockOffset < 0)
    {
        // 1.10's ConditionalPopulateNavOctree (0x1777740) leaves the lock to the element registration, which tests it
        // ("test byte [rcx+3FAh], 1", 0x178FFD7) before logging that it ignores the element.
        const uintptr_t Ignore = First(Finder::StringRefs(L"IGNORE(RegisterNavOctreeElement) %s"));
        const uintptr_t Register = Ignore ? Finder::FunctionStart(Ignore) : 0;
        for (uintptr_t Site = Register; Register && Site + 7 <= Ignore && OctreeLockOffset < 0; ++Site)
        {
            const uint8_t ModRm = Byte(Site + 1);
            if (Byte(Site) == 0xF6 && (ModRm & 0xF8) == 0x80 && (ModRm & 0x07) != 0x04 && Byte(Site + 6) == 0x01)
            {
                OctreeLockOffset = *reinterpret_cast<const int32_t*>(Site + 2);
            }
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
    // Build spawns the agents' missing navigation data and registers the queue before it rebuilds: the virtual call after
    // the lock test is SpawnMissingNavigationData, the call right after it ProcessRegistrationCandidates.
    Natives::UNavigationSystem_SpawnMissingNavigationDataIndex = SpawnMissingSlot;
    Natives::UNavigationSystem_ProcessRegistrationCandidatesIndex = ProcessCandidatesSlot;
    Natives::UNavigationSystem_ProcessRegistrationCandidates = ProcessCandidatesSlot < 0 && IsInText(ProcessCandidatesTarget)
        ? reinterpret_cast<decltype(Natives::UNavigationSystem_ProcessRegistrationCandidates)>(ProcessCandidatesTarget) : nullptr;
    Log::Info(L"Finder: UNavigationSystem::Build is slot 0x{:X}, NavBuildingLockFlags +0x{:X}, RebuildAll +0x{:X}, ConditionalPopulateNavOctree +0x{:X}, bNavOctreeLock +0x{:X}, SpawnMissingNavigationData slot 0x{:X}, ProcessRegistrationCandidates {}",
        BuildSlot, LockFlagsOffset, RebuildAll - Finder::ImageBase(), PopulateNavOctree - Finder::ImageBase(), OctreeLockOffset, SpawnMissingSlot,
        ProcessCandidatesSlot >= 0 ? std::format(L"slot 0x{:X}", ProcessCandidatesSlot) : Natives::UNavigationSystem_ProcessRegistrationCandidates ? std::format(L"+0x{:X}", ProcessCandidatesTarget - Finder::ImageBase()) : std::wstring(L"not found"));
    return true;
}

bool Finder::ResolveNavigationSystemInitializeForWorldLive(ptrdiff_t WorldNavigationSystemOffset)
{
    if (Natives::UNavigationSystem_InitializeForWorld)
    {
        return true;
    }
    static bool bSearched = false;
    if (bSearched || WorldNavigationSystemOffset <= 0)
    {
        return false;
    }
    bSearched = true;

    // UNavigationSystem::InitializeForWorld(World, Mode) has no strings (3.1 0x1E1CCC0, 3.0 0x1E08210; UEngine::LoadMap
    // calls it with "mov dl, 1" after InitializeActorsForPlay):
    //   test rcx, rcx; jz; mov [rsp+8], rbx; push rdi; sub rsp, 20h; mov rbx, rcx; movzx edi, dl;
    //   mov rcx, [rcx+World.NavigationSystem]; test rcx, rcx; jnz; mov rcx, rbx; call CreateNavigationSystem
    // then calls the system's OnWorldInitDone(Mode). The only function in the exe that starts this way.
    const int32_t Offset = static_cast<int32_t>(WorldNavigationSystemOffset);
    const std::initializer_list<int32_t> Pattern = { 0x48, 0x85, 0xC9, 0x74, -1, 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x0F, 0xB6, 0xFA,
        0x48, 0x8B, 0x89, Offset & 0xFF, (Offset >> 8) & 0xFF, (Offset >> 16) & 0xFF, (Offset >> 24) & 0xFF, 0x48, 0x85, 0xC9, 0x75, -1, 0x48, 0x8B, 0xCB, 0xE8 };
    uintptr_t Found = 0;
    int32_t Matches = 0;
    for (const auto& [Begin, End] : Finder::ExecutableSections())
    {
        for (uintptr_t Site = FindInstruction(Begin, End, Pattern); Site; Site = FindInstruction(Site + 1, End, Pattern))
        {
            Found = Site;
            ++Matches;
        }
    }
    if (Matches != 1)
    {
        Log::Error(L"Finder: UNavigationSystem::InitializeForWorld not found ({} matches)", Matches);
        return false;
    }
    Natives::UNavigationSystem_InitializeForWorld = reinterpret_cast<decltype(Natives::UNavigationSystem_InitializeForWorld)>(Found);
    Log::Info(L"Finder: UNavigationSystem::InitializeForWorld at 0x{:X}", Found - Finder::ImageBase());
    return true;
}

bool Finder::ResolveAddNavigationSystemToWorldLive(ptrdiff_t WorldNavigationSystemOffset, ptrdiff_t NavigationSystemConfigOffset)
{
    if (Natives::FNavigationSystem_AddNavigationSystemToWorld)
    {
        return true;
    }
    static bool bSearched = false;
    if (bSearched || WorldNavigationSystemOffset <= 0 || NavigationSystemConfigOffset <= 0)
    {
        return false;
    }
    bSearched = true;

    // 4.2's FNavigationSystem::AddNavigationSystemToWorld(UWorld& WorldOwner, FNavigationSystemRunMode RunMode, bool
    // bInitializeForWorld) (0x1E9D8F0; UE 4.20's NavigationSystemBase.cpp, called by UEngine::LoadMap and UWorld::InitWorld)
    // has no strings either:
    //   mov [rsp+8], rbx; mov [rsp+10h], rsi; push rdi; sub rsp, 20h; cmp qword ptr [rcx+World.NavigationSystem], 0;
    //   movzx esi, r8b; movzx edi, dl; mov rbx, rcx; jnz; mov r8b, 1; xor edx, edx; call UWorld::GetWorldSettings;
    //   test rax, rax; jz; mov rcx, [rax+WorldSettings.NavigationSystemConfig]
    // then UNavigationSystemConfig::CreateAndConfigureNavigationSystem(World) (0x1EA1E60), UWorld::SetNavigationSystem
    // (0x24A7890) and the system's InitializeForWorld(World, RunMode).
    const auto Bytes = [](ptrdiff_t Value, int32_t Index) { return static_cast<int32_t>((Value >> (8 * Index)) & 0xFF); };
    const ptrdiff_t World = WorldNavigationSystemOffset;
    const ptrdiff_t Config = NavigationSystemConfigOffset;
    const std::initializer_list<int32_t> Pattern = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20,
        0x48, 0x83, 0xB9, Bytes(World, 0), Bytes(World, 1), Bytes(World, 2), Bytes(World, 3), 0x00, 0x41, 0x0F, 0xB6, 0xF0, 0x0F, 0xB6, 0xFA, 0x48, 0x8B, 0xD9, 0x75, -1,
        0x41, 0xB0, 0x01, 0x33, 0xD2, 0xE8, -1, -1, -1, -1, 0x48, 0x85, 0xC0, 0x74, -1, 0x48, 0x8B, 0x88, Bytes(Config, 0), Bytes(Config, 1), Bytes(Config, 2), Bytes(Config, 3) };
    // 6.00 (0x232AFD0) takes the config third, AddNavigationSystemToWorld(UWorld&, RunMode, UNavigationSystemConfig*,
    // bInitializeForWorld), and falls back on the world settings' NavigationSystemConfigOverride, then their config:
    //   ...; cmp qword ptr [rcx+World.NavigationSystem], 0; movzx esi, r9b; movzx edi, dl; mov rbx, rcx; jnz;
    //   test r8, r8; jnz; mov r8b, 1; xor edx, edx; call UWorld::GetWorldSettings; test rax, rax; jz;
    //   mov r8, [rax+NavigationSystemConfigOverride]; test r8, r8; jnz; mov r8, [rax+WorldSettings.NavigationSystemConfig]
    const std::initializer_list<int32_t> ConfigPattern = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20,
        0x48, 0x83, 0xB9, Bytes(World, 0), Bytes(World, 1), Bytes(World, 2), Bytes(World, 3), 0x00, 0x41, 0x0F, 0xB6, 0xF1, 0x0F, 0xB6, 0xFA, 0x48, 0x8B, 0xD9, 0x75, -1,
        0x4D, 0x85, 0xC0, 0x75, -1, 0x41, 0xB0, 0x01, 0x33, 0xD2, 0xE8, -1, -1, -1, -1, 0x48, 0x85, 0xC0, 0x74, -1, 0x4C, 0x8B, 0x80, -1, -1, -1, -1,
        0x4D, 0x85, 0xC0, 0x75, -1, 0x4C, 0x8B, 0x80, Bytes(Config, 0), Bytes(Config, 1), Bytes(Config, 2), Bytes(Config, 3) };
    uintptr_t Found = 0;
    int32_t Matches = 0;
    bool bTakesConfig = false;
    for (const auto* Form : { &Pattern, &ConfigPattern })
    {
        for (const auto& [Begin, End] : Finder::ExecutableSections())
        {
            for (uintptr_t Site = FindInstruction(Begin, End, *Form); Site; Site = FindInstruction(Site + 1, End, *Form))
            {
                Found = Site;
                ++Matches;
            }
        }
        if (Matches)
        {
            bTakesConfig = Form == &ConfigPattern;
            break;
        }
    }
    if (Matches != 1)
    {
        Log::Error(L"Finder: FNavigationSystem::AddNavigationSystemToWorld not found ({} matches)", Matches);
        return false;
    }
    Natives::FNavigationSystem_AddNavigationSystemToWorld = reinterpret_cast<decltype(Natives::FNavigationSystem_AddNavigationSystemToWorld)>(Found);
    Natives::FNavigationSystem_AddNavigationSystemToWorldTakesConfig = bTakesConfig;
    Log::Info(L"Finder: FNavigationSystem::AddNavigationSystemToWorld at 0x{:X}{}", Found - Finder::ImageBase(), bTakesConfig ? L" (takes the config)" : L"");
    return true;
}

bool Finder::ResolveRecastNavMeshLive()
{
    if (Natives::dtNavMesh_addTile)
    {
        return true;
    }
    static bool bSearched = false;
    if (bSearched)
    {
        return false;
    }
    bSearched = true;

    // FRecastNavMeshGenerator::AddGeneratedTiles (2.5 0x1DDF0A0, 3.0 0x1E21D80, 3.1 0x1E36B20, 3.2 0x1E65480) logs
    // "%s> Tile (%d,%d:%d), tile limit reached!! (%d)" when dtNavMesh::addTile runs out of tiles. The same bytes in all four:
    //   mov rax, [r15+DestNavMesh]; mov rcx, [rax+RecastNavMeshImpl]; mov rax, [rcx+DetourNavMesh]
    //   test byte ptr [r15+270h], 2 (bRestrictBuildingToActiveTiles); mov r8, [r15+ActiveTiles]; movsxd rax, [r15+ActiveTiles+8]
    //   call dtNavMesh::addTile; mov ecx, eax; and eax, 80000004h; cmp eax, 80000004h (DT_FAILURE | DT_OUT_OF_MEMORY)
    //   inc dword ptr [r15+NumActiveTiles] (after a tile is added)
    // 3.5.1's (0x1F51650) keeps the generator in r13 and the Detour mesh in r15 ("mov r15, [rcx+8]", 0x1F51758), and
    // keeps addTile's status in ebx ("mov ebx, eax"); the offsets are 3.3's.
    const uintptr_t Reference = First(Finder::StringRefs(L"%s> Tile (%d,%d:%d), tile limit reached!! (%d)"));
    const uintptr_t Function = Reference ? Finder::FunctionStart(Reference) : 0;
    const uintptr_t End = Function ? (std::max)(Finder::FunctionEnd(Function), Reference + 0x400) : 0;
    // "mov rax, [Generator + disp32]" (REX.WB, mod 10, reg rax, an r8-r15 base that isn't r12), then the two loads.
    uintptr_t Impl = 0;
    for (uintptr_t Site = Function ? FindInstruction(Function, End, { 0x49, 0x8B, -1, -1, -1, -1, -1, 0x48, 0x8B, 0x88, -1, -1, -1, -1, -1, 0x8B, -1, -1 }) : 0; Site && !Impl;
         Site = FindInstruction(Site + 1, End, { 0x49, 0x8B, -1, -1, -1, -1, -1, 0x48, 0x8B, 0x88, -1, -1, -1, -1, -1, 0x8B, -1, -1 }))
    {
        const bool bGeneratorLoad = (Byte(Site + 2) & 0xF8) == 0x80 && (Byte(Site + 2) & 0x07) != 0x04;
        const bool bDetourLoad = (Byte(Site + 14) == 0x48 || Byte(Site + 14) == 0x4C) && (Byte(Site + 16) & 0xC7) == 0x41;
        Impl = bGeneratorLoad && bDetourLoad ? Site : 0;
    }
    // The three loads' displacements (back to back up to 4.1).
    int32_t ImplOffset = Impl ? *reinterpret_cast<const int32_t*>(Impl + 10) : -1;
    int32_t DetourOffset = Impl ? Byte(Impl + 17) : -1;
    uintptr_t ActiveSearchEnd = Impl + 0x40;
    // 4.2's (0x26EC750) schedules other instructions between them: mov rax, [r13+0D8h] (0x26EC860); mov edi, ebx; mov
    // [rsp+68h], rdx; mov rcx, [rax+580h]; mov eax, ebx; mov r14, [rcx+8] (0x26EC877), and its ActiveTiles loads come
    // 0x90 bytes on (0x26EC8F0).
    for (uintptr_t Site = Function && !Impl ? FindInstruction(Function, End, { 0x49, 0x8B, -1, -1, -1, -1, -1 }) : 0; Site && !Impl; Site = FindInstruction(Site + 1, End, { 0x49, 0x8B, -1, -1, -1, -1, -1 }))
    {
        if ((Byte(Site + 2) & 0xF8) != 0x80 || (Byte(Site + 2) & 0x07) == 0x04)
        {
            continue;
        }
        const uintptr_t ImplLoad = FindInstruction(Site + 7, Site + 0x18, { 0x48, 0x8B, 0x88, -1, -1, -1, -1 });
        uintptr_t DetourLoad = ImplLoad ? FindInstruction(ImplLoad + 7, ImplLoad + 0x18, { 0x4C, 0x8B, -1, -1 }) : 0;
        if (DetourLoad && (Byte(DetourLoad + 2) & 0xC7) != 0x41)
        {
            DetourLoad = 0;
        }
        if (DetourLoad)
        {
            Impl = Site;
            ImplOffset = *reinterpret_cast<const int32_t*>(ImplLoad + 3);
            DetourOffset = Byte(DetourLoad + 3);
            ActiveSearchEnd = Site + 0x100;
        }
    }
    // The generator's base register, as the mod-10 ModRM byte with reg 0 (r15 0x87, r13 0x85).
    const int32_t Generator = Impl ? Byte(Impl + 2) : -1;
    const uintptr_t Active = Impl ? FindInstruction(Impl, ActiveSearchEnd, { 0x4D, 0x8B, Generator, -1, -1, -1, -1, 0x49, 0x63, Generator }) : 0;
    uintptr_t AddTileCall = 0;
    for (uintptr_t Site = Function ? FindInstruction(Function, End, { 0xE8, -1, -1, -1, -1, 0x8B, -1, 0x25, 0x04, 0x00, 0x00, 0x80, 0x3D, 0x04, 0x00, 0x00, 0x80 }) : 0; Site && !AddTileCall;
         Site = FindInstruction(Site + 1, End, { 0xE8, -1, -1, -1, -1, 0x8B, -1, 0x25, 0x04, 0x00, 0x00, 0x80, 0x3D, 0x04, 0x00, 0x00, 0x80 }))
    {
        // "mov ecx, eax" (2.5-3.3) or "mov ebx, eax" (3.5.1): a register-to-register copy of eax.
        AddTileCall = (Byte(Site + 6) & 0xC7) == 0xC0 ? Site : 0;
    }
    const uintptr_t AddTile = AddTileCall ? Finder::RelativeTarget(AddTileCall, 1, 5) : 0;
    const uintptr_t Added = AddTileCall && Impl ? FindInstruction(AddTileCall, End, { 0x41, 0xFF, Generator, -1, -1, -1, -1 }) : 0;
    if (!Impl || !Active || !IsInText(AddTile) || !Added)
    {
        Log::Error(L"Finder: dtNavMesh::addTile not resolved (AddGeneratedTiles {}, impl {}, active tiles {}, add tile {}, tile count {})", Function != 0, Impl != 0, Active != 0, IsInText(AddTile), Added != 0);
        return false;
    }
    Natives::FRecastNavMeshGenerator_DestNavMesh = *reinterpret_cast<const int32_t*>(Impl + 3);
    Natives::ARecastNavMesh_RecastNavMeshImpl = ImplOffset;
    Natives::FPImplRecastNavMesh_DetourNavMesh = DetourOffset;
    Natives::FRecastNavMeshGenerator_ActiveTiles = *reinterpret_cast<const int32_t*>(Active + 3);
    Natives::FRecastNavMeshGenerator_NumActiveTiles = *reinterpret_cast<const int32_t*>(Added + 3);
    Natives::dtNavMesh_addTile = reinterpret_cast<decltype(Natives::dtNavMesh_addTile)>(AddTile);
    Log::Info(L"Finder: dtNavMesh::addTile at 0x{:X}; RecastNavMeshImpl +0x{:X}, DetourNavMesh +0x{:X}, generator DestNavMesh +0x{:X}, ActiveTiles +0x{:X}, NumActiveTiles +0x{:X}", AddTile - Finder::ImageBase(),
        Natives::ARecastNavMesh_RecastNavMeshImpl, Natives::FPImplRecastNavMesh_DetourNavMesh, Natives::FRecastNavMeshGenerator_DestNavMesh, Natives::FRecastNavMeshGenerator_ActiveTiles, Natives::FRecastNavMeshGenerator_NumActiveTiles);
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
        return Finder::BodyIs(Target, { Opcode, 0xC0, 0xC3 });
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
    // 5.0 (GrantScoreReward 0xDC1BC0 calls it at 0xDC1FB8) and 5.10 (0xDD26D0, at 0xDD2AC8) inline the TSharedFromThis
    // weak reference update (4.5 calls it, 0x459110), so the maker grew from 0x5E bytes (4.5 0xF9E3A0) to 0xE3 (5.0
    // 0x1141DD0, 5.10 0x11585E0). The larger size is only tried when the old one found nothing, so 1.1-4.5 match as before.
    const auto IsContextMaker = [](uintptr_t Target, uintptr_t MaxSize)
    {
        const uintptr_t TargetEnd = Finder::FunctionStart(Target) == Target ? Finder::FunctionEnd(Target) : 0;
        return TargetEnd > Target && TargetEnd - Target < MaxSize && FindInstruction(Target, TargetEnd, { 0xB9, 0x20, 0x00, 0x00, 0x00, 0xE8 }) != 0;
    };
    uintptr_t GrowCall = 0;
    uintptr_t GrowTarget = 0;
    for (const auto& [Call, Target] : Calls)
    {
        if (CriticalLoad && Call > ScoreCall && Call < CriticalLoad)
        {
            GrowCall = Call;
            GrowTarget = Target;
        }
    }
    uintptr_t ContextCall = 0;
    uintptr_t ContextTarget = 0;
    for (const uintptr_t MaxSize : { uintptr_t{ 0x80 }, uintptr_t{ 0x100 } })
    {
        for (const auto& [Call, Target] : Calls)
        {
            if (CriticalLoad && !ContextCall && Call > CriticalLoad && IsContextMaker(Target, MaxSize))
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
                if (IsContextMaker(InnerTarget, MaxSize))
                {
                    ContextCall = InnerCall;
                    ContextTarget = InnerTarget;
                    break;
                }
            }
        }
        if (ContextCall)
        {
            break;
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

bool Finder::ResolveMatchEndScoresLive(FEarnScoreCallSites& OutSites)
{
    const auto IsStub = [](uintptr_t Target, uint8_t Opcode)
    {
        return Finder::BodyIs(Target, { Opcode, 0xC0, 0xC3 });
    };

    // 5.21's AFortGameMode::SendEarnScore (slot 0x140, 0xE07650) has the scores built by a callee (0xDF6220): the one
    // that reads the total score stat (lea edx, [r8+1Bh], GetStat 27) and calls one 32 C0 C3 stub (the earn gate) and
    // one 33 C0 C3 stub (the badges score), as 1.1's SendEarnScore did itself. The virtual call after it is the grant
    // (call [rax+9D0h], slot 0x13A), which gets the scores and bIsFinal.
    void** GameModeVTable = CdoVTable(L"/Script/FortniteGame.FortGameMode");
    uintptr_t Scores = 0;
    uintptr_t ScoresEnd = 0;
    int32_t GrantIndex = -1;
    const int32_t Index = FirstVTableIndexWhere(GameModeVTable, [&](uintptr_t Entry)
    {
        const uintptr_t EntryEnd = Finder::FunctionStart(Entry) == Entry ? Finder::FunctionEnd(Entry) : 0;
        if (EntryEnd <= Entry || EntryEnd - Entry > 0x400)
        {
            return false;
        }
        for (const auto& [Call, Target] : Finder::DirectCalls(Entry, EntryEnd))
        {
            const uintptr_t TargetEnd = IsInText(Target) && Finder::FunctionStart(Target) == Target ? Finder::FunctionEnd(Target) : 0;
            if (TargetEnd <= Target || TargetEnd - Target > 0x2000 || !FindInstruction(Target, TargetEnd, { 0x41, 0x8D, 0x50, 0x1B }))
            {
                continue;
            }
            int32_t Gates = 0;
            int32_t BadgeScores = 0;
            for (const auto& [InnerCall, InnerTarget] : Finder::DirectCalls(Target, TargetEnd))
            {
                Gates += IsStub(InnerTarget, 0x32) ? 1 : 0;
                BadgeScores += IsStub(InnerTarget, 0x33) ? 1 : 0;
            }
            const int32_t Grant = VirtualSlot(FindVirtualCall(Call + 5, EntryEnd));
            if (Gates == 1 && BadgeScores == 1 && Grant >= 0)
            {
                Scores = Target;
                ScoresEnd = TargetEnd;
                GrantIndex = Grant;
                return true;
            }
        }
        return false;
    });

    uintptr_t GateCall = 0;
    uintptr_t ScoreCall = 0;
    for (const auto& [Call, Target] : Scores ? Finder::DirectCalls(Scores, ScoresEnd) : decltype(Finder::DirectCalls(0, 0)){})
    {
        GateCall = IsStub(Target, 0x32) ? Call : GateCall;
        ScoreCall = IsStub(Target, 0x33) ? Call : ScoreCall;
    }
    const uintptr_t TotalScoreLoad = Scores ? FindInstruction(Scores, ScoresEnd, { 0x41, 0x8D, 0x50, 0x1B }) : 0;
    const int32_t GetStatIndex = TotalScoreLoad ? VirtualSlot(FindVirtualCall(TotalScoreLoad + 4, TotalScoreLoad + 0x10)) : -1;
    if (Index < 0 || !GateCall || !ScoreCall || GetStatIndex < 0)
    {
        Log::Error(L"Finder: AFortGameMode match end scores not decoded (SendEarnScore slot {}, scores 0x{:X}, grant slot {}, gate {}, score {}, GetStat slot {})", Index, Scores ? Scores - Finder::ImageBase() : 0, GrantIndex, GateCall != 0, ScoreCall != 0, GetStatIndex);
        return false;
    }

    OutSites.GateCall = reinterpret_cast<uint8_t*>(GateCall);
    OutSites.ScoreCall = reinterpret_cast<uint8_t*>(ScoreCall);
    Natives::AFortGameMode_GetPlayersAndScores = reinterpret_cast<decltype(Natives::AFortGameMode_GetPlayersAndScores)>(Scores);
    Natives::AFortGameMode_GrantScoreRewardIndex = GrantIndex;
    Natives::AFortPlayerState_GetStatIndex = GetStatIndex;
    Log::Info(L"Finder: AFortGameMode::SendEarnScore is slot 0x{:X}: scores 0x{:X}, grant slot 0x{:X}, AFortPlayerState::GetStat slot 0x{:X}", Index, Scores - Finder::ImageBase(), GrantIndex, GetStatIndex);
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
    // 2.4.2 splits the add out into the virtual InitializeExistingItemInternal (0x6AD8E0), which tail-jumps to
    // UpdateItemInstances; InitializeExistingItem (0x6AD760) keeps the bAllowMcpItemCreation check and the guid lookup
    // and calls it through the vtable (call [rax+5F0h]). It is the callee of UFortWorldItem::PopulateFromMcpItem
    // (0x6D81F0, call at 0x6D849D) that makes that virtual call.
    if (!InitializeExistingItem && !bAmbiguous && UpdateItemInstances)
    {
        uintptr_t InitializeExistingItemInternal = 0;
        for (uintptr_t Jump : Finder::CallsTo(UpdateItemInstances, true))
        {
            const uintptr_t Caller = Byte(Jump) == 0xE9 ? Finder::FunctionStart(Jump) : 0;
            if (Caller && Caller != Update)
            {
                bAmbiguous |= InitializeExistingItemInternal && InitializeExistingItemInternal != Caller;
                InitializeExistingItemInternal = Caller;
            }
        }
        UClass* InventoryClass = InitializeExistingItemInternal && !bAmbiguous ? FindObject<UClass>(nullptr, L"/Script/FortniteGame.FortInventory") : nullptr;
        UObject* InventoryDefault = InventoryClass ? InventoryClass->GetDefaultObject() : nullptr;
        void** const InventoryVTable = InventoryDefault ? *reinterpret_cast<void***>(InventoryDefault) : nullptr;
        const int32_t InternalSlot = FirstVTableIndex(InventoryVTable, InitializeExistingItemInternal);
        const uintptr_t PopulateFromMcpItem = InternalSlot >= 0 ? FunctionOf(L"UFortWorldItem::PopulateFromMcpItem: Got mcp request to refresh item %s for %s but it has local changes, ignoring mcp.") : 0;
        const uintptr_t PopulateFromMcpItemEnd = PopulateFromMcpItem ? Finder::FunctionEnd(PopulateFromMcpItem) : 0;
        for (const auto& [Call, Target] : PopulateFromMcpItemEnd > PopulateFromMcpItem ? Finder::DirectCalls(PopulateFromMcpItem, PopulateFromMcpItemEnd) : std::vector<std::pair<uintptr_t, uintptr_t>>{})
        {
            const uintptr_t TargetEnd = Target != InitializeExistingItem && Finder::FunctionStart(Target) == Target ? Finder::FunctionEnd(Target) : 0;
            // call [r64+disp32]: FF /2 with mod 10 (not rsp/r12-based).
            for (uintptr_t Site = Target; TargetEnd && Site + 6 <= TargetEnd; ++Site)
            {
                if (Byte(Site) == 0xFF && (Byte(Site + 1) & 0xF8) == 0x90 && Byte(Site + 1) != 0x94 && *reinterpret_cast<const int32_t*>(Site + 2) == InternalSlot * static_cast<int32_t>(sizeof(void*)))
                {
                    bAmbiguous |= InitializeExistingItem != 0;
                    InitializeExistingItem = Target;
                    break;
                }
            }
        }
    }
    if (!InitializeExistingItem || bAmbiguous)
    {
        Log::Error(L"Finder: AFortInventory::InitializeExistingItem not found");
        return false;
    }

    Natives::AFortInventory_InitializeExistingItem = reinterpret_cast<decltype(Natives::AFortInventory_InitializeExistingItem)>(InitializeExistingItem);
    Log::Info(L"Finder: AFortInventory::InitializeExistingItem found");

    // 1.11 adds an item it doesn't have only while the inventory's bAllowMcpItemCreation is set (0x6BA3D4:
    // `cmp byte ptr [rbx+0x4EB], bpl; je` to the return). 1.10 and earlier have no such check. 2.4.2 tests it first
    // (0x6AD76A: `cmp byte ptr [rcx+0x4EB], 0`, two movs, then je).
    const uintptr_t InitializeExistingItemEnd = Finder::FunctionEnd(InitializeExistingItem);
    for (uintptr_t Site = InitializeExistingItem; Site && Site + 13 <= InitializeExistingItemEnd; ++Site)
    {
        const uint8_t ModRM = Byte(Site + 2);
        const uint8_t CmpModRM = Byte(Site + 1);
        const bool bCmpRegister = (Byte(Site) & 0xF0) == 0x40 && Byte(Site + 1) == 0x38 && (ModRM & 0xC0) == 0x80 && (ModRM & 0x07) != 0x04
            && Byte(Site + 7) == 0x0F && Byte(Site + 8) == 0x84;
        const bool bCmpZero = Byte(Site) == 0x80 && (CmpModRM & 0xF8) == 0xB8 && (CmpModRM & 0x07) != 0x04 && Byte(Site + 6) == 0x00
            && FindInstruction(Site + 7, std::min(Site + 0x17, InitializeExistingItemEnd), { 0x0F, 0x84 });
        if (bCmpZero)
        {
            Natives::AFortInventory_bAllowMcpItemCreation = *reinterpret_cast<const int32_t*>(Site + 2);
            Log::Info(L"Finder: AFortInventory::bAllowMcpItemCreation is +0x{:X}", Natives::AFortInventory_bAllowMcpItemCreation);
            break;
        }
        if (bCmpRegister)
        {
            Natives::AFortInventory_bAllowMcpItemCreation = *reinterpret_cast<const int32_t*>(Site + 3);
            Log::Info(L"Finder: AFortInventory::bAllowMcpItemCreation is +0x{:X}", Natives::AFortInventory_bAllowMcpItemCreation);
            break;
        }
    }
    return true;
}

bool Finder::ResolveBackpackLive(ptrdiff_t PrimaryPickupItemEntryOffset, ptrdiff_t WorldItemEntryOffset)
{
    if (Natives::AFortInventory_CanStackWith || Natives::FFortItemEntry_IsSameItem)
    {
        return true;
    }
    // Every inventory change asks again; a build where the search fails logged it on each one (5.0 #345).
    static bool bSearched = false;
    if (bSearched)
    {
        return false;
    }
    bSearched = true;

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

    // 1.8 moved both limits into functions taking (AFortPlayerController*, EFortInventoryType, ItemEntry); the two
    // callees are now small wrappers (Owner cast, InventoryType byte) that tail-jump to them (0x68A2B0 -> 0x68A320,
    // 0x68A730 -> 0x68A7A0). Look through such a wrapper.
    const auto Unwrap = [](uintptr_t Function)
    {
        const uintptr_t FunctionEnd = IsInText(Function) ? Finder::FunctionEnd(Function) : 0;
        const uintptr_t Target = FunctionEnd > Function && FunctionEnd - Function < 0x80 && Byte(FunctionEnd - 5) == 0xE9 ? Finder::RelativeTarget(FunctionEnd - 5, 1, 5) : 0;
        return IsInText(Target) && Finder::FunctionStart(Target) == Target ? Target : Function;
    };
    const std::vector<uintptr_t> ReasonCalls = Targets(OverflowReason);
    const uintptr_t StackLimitCall = ReasonCalls.size() >= 2 ? ReasonCalls[ReasonCalls.size() - 2] : 0;
    const uintptr_t BackpackLimitCall = ReasonCalls.size() >= 2 ? ReasonCalls.back() : 0;

    // 1.9 (CL-3757339) gives "Backpack full!" (0x6AF4D0) without an overflow reason: the reason and the limit wrappers
    // are gone, and the limits take (ItemEntry, the owner's inventory owner interface, InventoryType). Its new capacity
    // function (0x684300) is the only one to log an overridden capacity; the backpack limit (0x6874E0) is the one
    // function calling it that caps at 512, and the overflow (0x686E80) the backpack limit's one caller. Of the
    // overflow's callees the stack limit (0x687080) is the one that tests a call's bool and shares a callee
    // (GetMaxStackSize) with the backpack limit. GetMaxStackSize and CanStackWith keep 1.8's shapes.
    // 2.4.2's backpack limit (0x6A7B10) caps at 500 (cmp eax, 1F4h at 0x6A7C39) instead of 512.
    // 2.5's overflow (0xC271C0) also calls the capacity function itself (0xC27252, against the item count), and the
    // capacity function has the stack limit's shape (a call's bool tested, a callee the backpack limit shares), so it is
    // no stack limit candidate: taking it made the pick ambiguous and every item add failed (#214). The stack limit
    // there is 0xC273A0.
    // 3.5.1's backpack limit (0xCD1DD0) loads the cap instead of comparing with it: mov eax, 1F4h (0xCD1FC9).
    const auto HasBackpackCap = [](uintptr_t Begin, uintptr_t End)
    {
        return FindInstruction(Begin, End, { 0x3D, 0x00, 0x02, 0x00, 0x00 }) || FindInstruction(Begin, End, { 0x3D, 0xF4, 0x01, 0x00, 0x00 })
            || FindInstruction(Begin, End, { 0xB8, 0xF4, 0x01, 0x00, 0x00 });
    };
    // 5.0's (CL-4204761 0xF147F0) loads it into ecx: mov ecx, 1F4h (0xF14B4F). Only looked for when no caller has the
    // older forms, so the older builds' pick stays as it was.
    const auto HasBackpackCapInEcx = [](uintptr_t Begin, uintptr_t End)
    {
        return FindInstruction(Begin, End, { 0xB9, 0xF4, 0x01, 0x00, 0x00 }) != 0;
    };
    const uintptr_t Capacity = OverflowReason ? 0 : FunctionOf(L"GetInventoryCapacity is returning an overridden value of %i");
    uintptr_t CapacityStackLimit = 0;
    uintptr_t CapacityBackpackLimit = 0;
    bool bCapacityLimitsAmbiguous = false;
    for (const bool bEcx : { false, true })
    {
        for (const uintptr_t Call : Capacity && !CapacityBackpackLimit && !bCapacityLimitsAmbiguous ? Finder::CallsTo(Capacity, false) : std::vector<uintptr_t>{})
        {
            const uintptr_t Caller = Finder::FunctionStart(Call);
            const uintptr_t CallerEnd = Caller ? Finder::FunctionEnd(Caller) : 0;
            if (CallerEnd > Caller && Caller != CapacityBackpackLimit && (bEcx ? HasBackpackCapInEcx(Caller, CallerEnd) : HasBackpackCap(Caller, CallerEnd)))
            {
                bCapacityLimitsAmbiguous |= CapacityBackpackLimit != 0;
                CapacityBackpackLimit = Caller;
            }
        }
    }

    // 5.0 inlines CanStackWith (4.5 0xD7A520) into the stack limit (0xF142B0): the world item's entry (0x7E61F0:
    // lea rax, [rcx+0D0h]; ret) goes to the item entries' same-item test (mov rdx, rax; mov rcx, rdi; call 0xF01CF0),
    // then the world item to its regen cooldown test (mov rcx, rbx; movzx r15d, al; call 0xF46890: xorps xmm0, xmm0;
    // comiss xmm0, [rcx+1A8h]; setb al; ret). Returns the same-item test and the cooldown's offset.
    // 5.40's stack limit (0xF73560) loads the new entry from its stack argument between the two: after the entry getter
    // (0x806050) comes "mov rbx, [rbp+30h]; mov rdx, rax; mov rcx, rbx; call 0xF91F30" (0xF73961), then the cooldown test
    // (0xF9F3C0, +0x1A8). That form is only looked for after 5.0's.
    const auto FindInlinedCanStackWith = [WorldItemEntryOffset](uintptr_t Function) -> std::pair<uintptr_t, ptrdiff_t>
    {
        const uintptr_t FunctionEnd = WorldItemEntryOffset > 0 && IsInText(Function) ? Finder::FunctionEnd(Function) : 0;
        for (const auto& [Call, Target] : FunctionEnd > Function ? Finder::DirectCalls(Function, FunctionEnd) : std::vector<std::pair<uintptr_t, uintptr_t>>{})
        {
            const bool bEntryGetter = Byte(Target) == 0x48 && Byte(Target + 1) == 0x8D && Byte(Target + 2) == 0x81
                && *reinterpret_cast<const int32_t*>(Target + 3) == WorldItemEntryOffset && Byte(Target + 7) == 0xC3;
            const auto IsSameItemCall = [FunctionEnd](uintptr_t Move)
            {
                return Move + 11 <= FunctionEnd && Byte(Move) == 0x48 && Byte(Move + 1) == 0x8B && Byte(Move + 2) == 0xD0 && Byte(Move + 6) == 0xE8;
            };
            // mov r64, [rbp+disp8] (48 8B, ModRM mod 01 and rm rbp), into neither rax nor rdx.
            const auto IsStackArgumentLoad = [FunctionEnd](uintptr_t Load)
            {
                const uint8_t ModRm = Byte(Load + 2);
                return Load + 4 <= FunctionEnd && Byte(Load) == 0x48 && Byte(Load + 1) == 0x8B && (ModRm & 0xC7) == 0x45 && (ModRm & 0x38) != 0x00 && (ModRm & 0x38) != 0x10;
            };
            const uintptr_t Next = Call + 5;
            const uintptr_t Move = !bEntryGetter ? 0 : IsSameItemCall(Next) ? Next : IsStackArgumentLoad(Next) && IsSameItemCall(Next + 4) ? Next + 4 : 0;
            if (!Move)
            {
                continue;
            }
            const uintptr_t SameItem = Finder::RelativeTarget(Move + 6, 1, 5);
            const uintptr_t CooldownCall = FindRawCall(Move + 11, std::min(Move + 11 + 0x10, FunctionEnd));
            const uintptr_t Cooldown = CooldownCall ? Finder::RelativeTarget(CooldownCall, 1, 5) : 0;
            if (IsInText(SameItem) && Finder::FunctionStart(SameItem) == SameItem && Cooldown && Finder::BodyIs(Cooldown, { 0x0F, 0x57, 0xC0, 0x0F, 0x2F, 0x81 })
                && Byte(Cooldown + 10) == 0x0F && Byte(Cooldown + 11) == 0x92 && Byte(Cooldown + 12) == 0xC0 && Byte(Cooldown + 13) == 0xC3)
            {
                return { SameItem, *reinterpret_cast<const int32_t*>(Cooldown + 6) };
            }
        }
        return { 0, -1 };
    };
    const std::vector<uintptr_t> CapacityBackpackCalls = CapacityBackpackLimit && !bCapacityLimitsAmbiguous ? Targets(CapacityBackpackLimit) : std::vector<uintptr_t>{};
    uintptr_t CapacityOverflow = 0;
    for (const uintptr_t Call : CapacityBackpackCalls.empty() ? std::vector<uintptr_t>{} : Finder::CallsTo(CapacityBackpackLimit, false))
    {
        const uintptr_t Caller = Finder::FunctionStart(Call);
        bCapacityLimitsAmbiguous |= CapacityOverflow && CapacityOverflow != Caller;
        CapacityOverflow = Caller;
    }
    // 5.0's stack limit tests no call's bool (CanStackWith is inlined into it): when no callee has that shape, the stack
    // limit is the one callee that shares a callee with the backpack limit, isn't one of the backpack limit's own callees
    // (5.0's item count, 0xF11930, is both) and has the inlined CanStackWith.
    std::vector<uintptr_t> InlinedStackLimits;
    for (const uintptr_t Callee : CapacityOverflow && !bCapacityLimitsAmbiguous ? Targets(CapacityOverflow) : std::vector<uintptr_t>{})
    {
        const uintptr_t CalleeEnd = Callee != CapacityBackpackLimit && Callee != CapacityStackLimit && Callee != Capacity && Finder::FunctionStart(Callee) == Callee ? Finder::FunctionEnd(Callee) : 0;
        bool bTestsBool = false;
        bool bSharesCallee = false;
        for (const auto& [Call, Target] : CalleeEnd > Callee ? Finder::DirectCalls(Callee, CalleeEnd) : std::vector<std::pair<uintptr_t, uintptr_t>>{})
        {
            bTestsBool |= Call + 7 <= CalleeEnd && Byte(Call + 5) == 0x84 && Byte(Call + 6) == 0xC0;
            bSharesCallee |= Finder::FunctionStart(Target) == Target && std::find(CapacityBackpackCalls.begin(), CapacityBackpackCalls.end(), Target) != CapacityBackpackCalls.end();
        }
        if (bTestsBool && bSharesCallee)
        {
            bCapacityLimitsAmbiguous |= CapacityStackLimit != 0;
            CapacityStackLimit = Callee;
        }
        else if (bSharesCallee && std::find(CapacityBackpackCalls.begin(), CapacityBackpackCalls.end(), Callee) == CapacityBackpackCalls.end()
            && std::find(InlinedStackLimits.begin(), InlinedStackLimits.end(), Callee) == InlinedStackLimits.end() && FindInlinedCanStackWith(Callee).first)
        {
            InlinedStackLimits.push_back(Callee);
        }
    }
    if (!CapacityStackLimit && !bCapacityLimitsAmbiguous && InlinedStackLimits.size() == 1)
    {
        CapacityStackLimit = InlinedStackLimits[0];
    }
    if (bCapacityLimitsAmbiguous || !CapacityStackLimit)
    {
        CapacityStackLimit = CapacityBackpackLimit = 0;
    }

    const uintptr_t StackLimit = OverflowReason ? Unwrap(StackLimitCall) : CapacityStackLimit;
    const uintptr_t BackpackLimit = OverflowReason ? Unwrap(BackpackLimitCall) : CapacityBackpackLimit;
    const bool bControllerLimits = OverflowReason ? StackLimit != StackLimitCall : StackLimit != 0;
    const uintptr_t StackLimitEnd = IsInText(StackLimit) ? Finder::FunctionEnd(StackLimit) : 0;
    const uintptr_t BackpackLimitEnd = IsInText(BackpackLimit) ? Finder::FunctionEnd(BackpackLimit) : 0;
    const bool bStackCap = BackpackLimitEnd > BackpackLimit && (HasBackpackCap(BackpackLimit, BackpackLimitEnd) || HasBackpackCapInEcx(BackpackLimit, BackpackLimitEnd));

    const std::vector<uintptr_t> StackLimitCalls = Targets(StackLimit);
    uintptr_t MaxStackSize = 0;
    for (const uintptr_t Target : Targets(BackpackLimit))
    {
        // 1.7.2's limits both also call a leaf getter (0x219A8F0, no .pdata entry) after GetMaxStackSize. 2.4's call
        // the item entry's count getter (0x1FECB70, 2.4.2 0x188B810: mov eax, [rcx+0Ch]; ret) after it, and it has one.
        const bool bFieldGetter = Byte(Target) == 0x8B && Byte(Target + 1) == 0x41 && Byte(Target + 3) == 0xC3;
        if (!bFieldGetter && Finder::FunctionStart(Target) == Target && std::find(StackLimitCalls.begin(), StackLimitCalls.end(), Target) != StackLimitCalls.end())
        {
            MaxStackSize = Target;
        }
    }
    // 3.5 inlines GetMaxStackSize(AFortPlayerController*, const UFortItemDefinition*) (3.5.1 0xCCF810: the controller's
    // hero, 0xC6DE50, then the item definition's virtual, call [rax+328h]) into both limits, so the callee they share
    // last is something else (0x79FBD0, a TArray's growth: GetMaxStackSize read the item count's call frame). The
    // function is still there: the small caller of a shared callee that makes the virtual call the backpack limit makes
    // right after calling it (0xCD1E9B, then 0xCD1EB0).
    const uintptr_t MaxStackSizeEnd = MaxStackSize ? Finder::FunctionEnd(MaxStackSize) : 0;
    if (bControllerLimits && BackpackLimitEnd > BackpackLimit && (!MaxStackSizeEnd || !FindVirtualCall(MaxStackSize, MaxStackSizeEnd)))
    {
        uintptr_t Inlined = 0;
        bool bInlinedAmbiguous = false;
        for (const auto& [Call, Target] : Finder::DirectCalls(BackpackLimit, BackpackLimitEnd))
        {
            const uintptr_t Virtual = std::find(StackLimitCalls.begin(), StackLimitCalls.end(), Target) != StackLimitCalls.end() ? FindVirtualCall(Call + 5, std::min(Call + 0x30, BackpackLimitEnd)) : 0;
            const uintptr_t NextCall = Virtual ? FindRawCall(Call + 5, Virtual) : 0;
            if (!Virtual || NextCall)
            {
                continue;
            }
            for (const uintptr_t Site : Finder::CallsTo(Target, false))
            {
                const uintptr_t Caller = Finder::FunctionStart(Site);
                const uintptr_t CallerEnd = Caller ? Finder::FunctionEnd(Caller) : 0;
                const uintptr_t CallerVirtual = CallerEnd > Caller && CallerEnd - Caller < 0x80 ? FindVirtualCall(Site + 5, CallerEnd) : 0;
                if (CallerVirtual && VirtualSlot(CallerVirtual) == VirtualSlot(Virtual) && Caller != Inlined)
                {
                    bInlinedAmbiguous |= Inlined != 0;
                    Inlined = Caller;
                }
            }
        }
        MaxStackSize = Inlined && !bInlinedAmbiguous ? Inlined : MaxStackSize;
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
    const auto [InlinedSameItem, BaseRegenCooldown] = !CanStackWith && StackLimit ? FindInlinedCanStackWith(StackLimit) : std::pair<uintptr_t, ptrdiff_t>{ 0, -1 };

    ptrdiff_t InterfaceOffset = -1;
    int32_t NumItemsSlot = -1;
    int32_t MaxItemsSlot = -1;
    for (uintptr_t Site = bControllerLimits ? 0 : BackpackLimit; Site && Site + 3 <= BackpackLimitEnd && MaxItemsSlot < 0; ++Site)
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
    // 1.8: the counts are read in BackpackLimit's helpers, through the inventory's interface:
    // lea rcx, [rax+388h]; mov rax, [rcx]; call [rax+30h] (NumItems, 0x687530) / call [rax+28h] (MaxItems, 0x686F0A).
    // The first displacement seen with two different slots is the interface.
    std::vector<std::pair<int32_t, int32_t>> InterfaceCalls;
    for (const uintptr_t Helper : bControllerLimits ? Targets(BackpackLimit) : std::vector<uintptr_t>{})
    {
        const uintptr_t HelperEnd = Finder::FunctionStart(Helper) == Helper ? Finder::FunctionEnd(Helper) : 0;
        for (uintptr_t Site = Helper; HelperEnd && Site + 13 <= HelperEnd && MaxItemsSlot < 0; ++Site)
        {
            const uint8_t ModRm = Byte(Site + 2);
            const int32_t Displacement = *reinterpret_cast<const int32_t*>(Site + 3);
            if (Byte(Site) != 0x48 || Byte(Site + 1) != 0x8D || (ModRm & 0xC0) != 0x80 || (ModRm & 0x07) == 0x04 || (ModRm & 0x38) != 0x08
                || Displacement <= 0 || Displacement >= 0x2000
                || Byte(Site + 7) != 0x48 || Byte(Site + 8) != 0x8B || Byte(Site + 9) != 0x01 || Byte(Site + 10) != 0xFF || Byte(Site + 11) != 0x50 || Byte(Site + 12) % 8 != 0)
            {
                continue;
            }
            const int32_t Slot = Byte(Site + 12) / 8;
            for (const auto& [SeenDisplacement, SeenSlot] : InterfaceCalls)
            {
                if (MaxItemsSlot < 0 && SeenDisplacement == Displacement && SeenSlot != Slot)
                {
                    InterfaceOffset = Displacement;
                    NumItemsSlot = SeenSlot;
                    MaxItemsSlot = Slot;
                }
            }
            InterfaceCalls.emplace_back(Displacement, Slot);
        }
        if (MaxItemsSlot >= 0)
        {
            break;
        }
    }
    // 1.9: only the count goes through the interface there (0x684960: lea rcx, [rax+388h]; mov rax, [rcx];
    // call [rax+30h]; GetMaxStackSize's call through the controller's interface has the same shape); the capacity function
    // is called directly. The interface's MaxItems (slot 5, 0x6845B0) is the first entry of AFortInventory's interface
    // vtable that tail-jumps to it.
    std::vector<std::pair<int32_t, int32_t>> CountCalls;
    for (const uintptr_t Helper : Capacity && bControllerLimits && MaxItemsSlot < 0 ? Targets(BackpackLimit) : std::vector<uintptr_t>{})
    {
        const uintptr_t HelperEnd = Helper != MaxStackSize && Helper != Capacity && Finder::FunctionStart(Helper) == Helper ? Finder::FunctionEnd(Helper) : 0;
        for (uintptr_t Site = Helper; HelperEnd && Site + 13 <= HelperEnd; ++Site)
        {
            const uint8_t ModRm = Byte(Site + 2);
            const int32_t Displacement = *reinterpret_cast<const int32_t*>(Site + 3);
            if (Byte(Site) == 0x48 && Byte(Site + 1) == 0x8D && (ModRm & 0xC0) == 0x80 && (ModRm & 0x07) != 0x04 && (ModRm & 0x38) == 0x08 && Displacement > 0 && Displacement < 0x2000
                && Byte(Site + 7) == 0x48 && Byte(Site + 8) == 0x8B && Byte(Site + 9) == 0x01 && Byte(Site + 10) == 0xFF && Byte(Site + 11) == 0x50 && Byte(Site + 12) % 8 == 0
                && std::find(CountCalls.begin(), CountCalls.end(), std::make_pair(Displacement, static_cast<int32_t>(Byte(Site + 12) / 8))) == CountCalls.end())
            {
                CountCalls.emplace_back(Displacement, Byte(Site + 12) / 8);
            }
        }
    }
    if (CountCalls.size() == 1)
    {
        UClass* InventoryClass = FindObject<UClass>(nullptr, L"/Script/FortniteGame.FortInventory");
        UObject* InventoryDefault = InventoryClass ? InventoryClass->GetDefaultObject() : nullptr;
        void** InterfaceVTable = InventoryDefault ? *reinterpret_cast<void***>(reinterpret_cast<uint8_t*>(InventoryDefault) + CountCalls[0].first) : nullptr;
        for (int32_t Slot = 0; InterfaceVTable && Slot < 0x40 && MaxItemsSlot < 0; ++Slot)
        {
            const uintptr_t Entry = VTableEntry(InterfaceVTable, Slot);
            if (!Entry)
            {
                break;
            }
            const uintptr_t EntryEnd = Finder::FunctionStart(Entry) == Entry ? Finder::FunctionEnd(Entry) : 0;
            if (Slot != CountCalls[0].second && EntryEnd > Entry + 5 && EntryEnd - Entry < 0x80 && Byte(EntryEnd - 5) == 0xE9 && Finder::RelativeTarget(EntryEnd - 5, 1, 5) == Capacity)
            {
                InterfaceOffset = CountCalls[0].first;
                NumItemsSlot = CountCalls[0].second;
                MaxItemsSlot = Slot;
            }
        }
    }

    // The inventory's overflow amount (Inventory, ItemEntry): the other function that calls both limits. 1.1 and 1.2's
    // (0x1405AAB60) takes the stack limit unless it is -1, else the backpack limit. 1.7.2's (0x630E80) checks the
    // stack limit only when the item already has a stack, which then returns the count that doesn't fit in the existing
    // stacks (never -1), and applies the backpack limit to what is left; the old pairing called on 1.7.2 overflows
    // every item that has no stack yet.
    uintptr_t Overflow = 0;
    for (const uintptr_t Call : StackLimit && BackpackLimit ? Finder::CallsTo(StackLimit, false) : std::vector<uintptr_t>{})
    {
        const uintptr_t Caller = Finder::FunctionStart(Call);
        const std::vector<uintptr_t> CallerCalls = Caller && Caller != OverflowReason && Caller != StackLimitCall ? Targets(Caller) : std::vector<uintptr_t>{};
        if (std::find(CallerCalls.begin(), CallerCalls.end(), BackpackLimit) != CallerCalls.end())
        {
            bAmbiguous |= Overflow && Overflow != Caller;
            Overflow = Caller;
        }
    }
    // 1.8: that caller is the (PlayerController, InventoryType, ItemEntry) overflow (0x689FE0); AFortInventory's
    // (Inventory, ItemEntry) entry point is the wrapper whose last instruction jumps to it (0x689F70).
    if (bControllerLimits && Overflow)
    {
        uintptr_t Wrapper = 0;
        for (const uintptr_t Jump : Finder::CallsTo(Overflow, true))
        {
            const uintptr_t Caller = Byte(Jump) == 0xE9 ? Finder::FunctionStart(Jump) : 0;
            if (Caller && Finder::FunctionEnd(Caller) == Jump + 5)
            {
                bAmbiguous |= Wrapper && Wrapper != Caller;
                Wrapper = Caller;
            }
        }
        Overflow = Wrapper;
    }

    const bool bInlinedCanStackWith = !CanStackWith && InlinedSameItem && BaseRegenCooldown > 0;
    if (!BackpackLimit || !StackLimit || !bStackCap || !MaxStackSize || (!CanStackWith && !bInlinedCanStackWith) || !Overflow || bAmbiguous || MaxItemsSlot < 0)
    {
        Log::Error(L"Finder: AFortInventory overflow checks not found (reason {}, capacity {}, stack cap {}, max stack {}, can stack {}, overflow {}, ambiguous {}, slots {}/{})", OverflowReason != 0, Capacity != 0, bStackCap, MaxStackSize != 0, CanStackWith != 0 || bInlinedCanStackWith, Overflow != 0, bAmbiguous, NumItemsSlot, MaxItemsSlot);
        return false;
    }

    if (bInlinedCanStackWith)
    {
        Natives::FFortItemEntry_IsSameItem = reinterpret_cast<decltype(Natives::FFortItemEntry_IsSameItem)>(InlinedSameItem);
        Natives::UFortWorldItem_BaseRegenCooldown = BaseRegenCooldown;
        Log::Info(L"Finder: AFortInventory::CanStackWith is inlined into the stack limit: the item entries' same-item test +0x{:X}, UFortWorldItem::BaseRegenCooldown +0x{:X}", InlinedSameItem - Finder::ImageBase(), BaseRegenCooldown);
    }
    Natives::AFortInventory_GetOverflowAmount = reinterpret_cast<decltype(Natives::AFortInventory_GetOverflowAmount)>(Overflow);
    Natives::AFortInventory_GetMaxStackSize = reinterpret_cast<decltype(Natives::AFortInventory_GetMaxStackSize)>(MaxStackSize);
    Natives::AFortInventory_InventoryInterfaceOffset = InterfaceOffset;
    Natives::IFortInventoryInterface_GetNumItemsIndex = NumItemsSlot;
    Natives::IFortInventoryInterface_GetMaxItemsIndex = MaxItemsSlot;
    Natives::AFortInventory_CanStackWith = reinterpret_cast<decltype(Natives::AFortInventory_CanStackWith)>(CanStackWith);
    Natives::AFortInventory_bControllerLimits = bControllerLimits;
    Log::Info(L"Finder: AFortInventory overflow checks found (overflow +0x{:X}, stack limit +0x{:X}, backpack limit +0x{:X}, interface +0x{:X} slots {}/{})", Overflow - Finder::ImageBase(), StackLimit - Finder::ImageBase(), BackpackLimit - Finder::ImageBase(), InterfaceOffset, NumItemsSlot, MaxItemsSlot);
    return true;
}

bool Finder::ResolveWorldItemCountLive(ptrdiff_t RegenCooldownOffset, ptrdiff_t ItemEntryOffset)
{
    if (Natives::UFortWorldItem_SetItemCountIndex >= 0)
    {
        return true;
    }

    // The opcode's last byte is its ModRM, which must address [reg+disp32]: comiss xmm0, [rdx+RegenCooldown] (0F 2F 82)
    // up to 3.6, comiss xmm0, [r13+RegenCooldown] (41 0F 2F 85) on 4.0.
    const auto FindDisplacement = [](uintptr_t Begin, uintptr_t End, std::initializer_list<int32_t> Opcode, ptrdiff_t Displacement)
    {
        for (uintptr_t Site = FindInstruction(Begin, End, Opcode); Site; Site = FindInstruction(Site + 1, End, Opcode))
        {
            const uint8_t ModRm = Byte(Site + Opcode.size() - 1);
            if ((ModRm & 0xC0) == 0x80 && (ModRm & 0x07) != 0x04 && Site + Opcode.size() + sizeof(int32_t) <= End && *reinterpret_cast<const int32_t*>(Site + Opcode.size()) == Displacement)
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
        const uintptr_t Compare = FunctionEnd > Function ? FindDisplacement(Function, FunctionEnd, { 0x0F, 0x2F, -1 }, RegenCooldownOffset) : 0;
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
    // lea reg, [reg+ItemEntry]: lea rdi, [rbx+...] (48 8D BB) up to 1.7.2, lea rcx, [rdi+...] (48 8D 8F) on 1.8.
    bool bLoadsItemEntry = false;
    for (uintptr_t Site = SetItemCountEnd > SetItemCount ? FindInstruction(SetItemCount, SetItemCountEnd, { 0x48, 0x8D }) : 0; Site && !bLoadsItemEntry; Site = FindInstruction(Site + 1, SetItemCountEnd, { 0x48, 0x8D }))
    {
        const uint8_t ModRm = Byte(Site + 2);
        bLoadsItemEntry = Site + 7 <= SetItemCountEnd && (ModRm & 0xC0) == 0x80 && (ModRm & 0x07) != 0x04 && *reinterpret_cast<const int32_t*>(Site + 3) == ItemEntryOffset;
    }
    const bool bSetItemCount = bLoadsItemEntry && FindInstruction(SetItemCount, SetItemCountEnd, { 0x80, 0x38, 0x03 });
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
    uintptr_t LastCall = 0;
    for (const auto& [Call, Target] : ImplementationEnd > Implementation ? Finder::DirectCalls(Implementation, ImplementationEnd) : std::vector<std::pair<uintptr_t, uintptr_t>>{})
    {
        Targets.push_back(Target);
        LastCall = Call;
    }
    uintptr_t SetStateValue = Targets.size() == 4 && Targets[0] == Targets[2] && Targets[1] == Targets[3] && Targets[0] != Targets[1] ? Targets[1] : 0;
    if (!SetStateValue && Targets.size() == 3)
    {
        // The call scan reads E8 bytes, and an E8 inside an instruction reads as a call too: 6.20's implementation
        // (0x12C00A0) clears its pending fabrication with mov qword ptr [rbx+1FE8h], 0 after SetStateValue, whose
        // displacement holds one (a "call" into the middle of a jz at 0x12C00F7). Read the function an instruction at a
        // time then, and keep the calls that start an instruction.
        std::vector<uintptr_t> InstructionTargets;
        uintptr_t LastInstructionCall = 0;
        for (uintptr_t Address = Implementation; Address && Address < ImplementationEnd;)
        {
            hde64s Instruction{};
            const uint32_t Length = hde64_disasm(reinterpret_cast<const void*>(Address), &Instruction);
            if (!Length || (Instruction.flags & F_ERROR))
            {
                InstructionTargets.clear();
                break;
            }
            if (Byte(Address) == 0xE8 && Length == 5)
            {
                InstructionTargets.push_back(Finder::RelativeTarget(Address, 1, 5));
                LastInstructionCall = Address;
            }
            Address += Length;
        }
        if (InstructionTargets.size() == 2)
        {
            Targets = InstructionTargets;
            LastCall = LastInstructionCall;
        }
    }
    if (!SetStateValue && Targets.size() == 2 && Targets[0] != Targets[1] && FindInstruction(LastCall - 0x10, LastCall, { 0xBA, 0x02, 0x00, 0x00, 0x00 }))
    {
        // 1.10 (0x8E0300) keeps one pending fabrication: its entry, then SetStateValue(Entry, 2, 1) (0x6AFFF0). 6.20
        // (0x12C00A0) the same, with lea r8d, [rdx-1] for the 1 after mov edx, 2.
        SetStateValue = Targets[1];
    }

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
    // 1.8 inlines MarkDirty at SetStateValue's end (its three calls, then bIsDirty). The standalone copy is the small
    // function making exactly those three calls whose first takes this+disp (mov rbx, rcx; add rcx, imm32; call).
    const auto StateCalls = !MarkDirty && SetStateValueEnd ? Finder::DirectCalls(SetStateValue, SetStateValueEnd) : std::vector<std::pair<uintptr_t, uintptr_t>>{};
    for (const uintptr_t Call : StateCalls.size() >= 3 ? Finder::CallsTo(StateCalls.back().second, false) : std::vector<uintptr_t>{})
    {
        const uintptr_t Caller = Finder::FunctionStart(Call);
        const uintptr_t CallerEnd = Caller ? Finder::FunctionEnd(Caller) : 0;
        const auto CallerCalls = CallerEnd > Caller && CallerEnd - Caller < 0x80 ? Finder::DirectCalls(Caller, CallerEnd) : std::vector<std::pair<uintptr_t, uintptr_t>>{};
        if (CallerCalls.size() == 3 && CallerCalls[0].second == StateCalls[StateCalls.size() - 3].second && CallerCalls[1].second == StateCalls[StateCalls.size() - 2].second
            && CallerCalls[2].second == StateCalls.back().second && CallerCalls[0].first >= Caller + 10
            && FindInstruction(CallerCalls[0].first - 10, CallerCalls[0].first, { 0x48, 0x8B, 0xD9, 0x48, 0x81, 0xC1 }) == CallerCalls[0].first - 10)
        {
            bAmbiguous |= MarkDirty && MarkDirty != Caller;
            MarkDirty = Caller;
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
    if ((Natives::AFortPlayerController_DisassembleInventoryItem || Natives::AFortPlayerController_DisassembleInventoryItems) && Natives::UFortWorldItemDefinition_GetDisassemblyResults)
    {
        return true;
    }

    const auto FirstCall = [](uintptr_t Function)
    {
        const uintptr_t FunctionEnd = IsInText(Function) ? Finder::FunctionEnd(Function) : 0;
        const uintptr_t Call = FunctionEnd ? FindRawCall(Function, FunctionEnd) : 0;
        return Call ? Finder::RelativeTarget(Call, 1, 5) : 0;
    };
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
    const auto Checks = [](uintptr_t Function, uintptr_t Check)
    {
        const uintptr_t FunctionEnd = Function && Check ? Finder::FunctionEnd(Function) : 0;
        return FunctionEnd > Function && CallWithin(Function, FunctionEnd - Function, Check);
    };

    UFunction* CanBeDisassembledFunction = FindObject<UFunction>(nullptr, L"/Script/FortniteGame.FortWorldItemDefinition:CanBeDisassembled");
    const uintptr_t CanBeDisassembled = FirstCall(CanBeDisassembledFunction ? reinterpret_cast<uintptr_t>(CanBeDisassembledFunction->GetNativeFunc()) : 0);

    void** PlayerControllerVTable = PlayerControllerDefault ? *reinterpret_cast<void***>(PlayerControllerDefault) : nullptr;
    const uintptr_t Implementation = VTableEntry(PlayerControllerVTable, ImplementationIndex(ServerDisassembleInventoryItem));
    uintptr_t Disassemble = FirstCall(Implementation);
    bool bBatch = false;
    if (Disassemble && !Checks(Disassemble, CanBeDisassembled))
    {
        // 1.10's ServerDisassembleInventoryItems_Implementation (0x89C730) pairs each GUID with its count (20 bytes)
        // first; its last call checks them (0x873910) and, off the server, sends them on. The server's part is stripped.
        Disassemble = LastCall(Implementation);
        bBatch = true;
    }
    const uintptr_t Results = FunctionOf(L"UFortWorldItemDefinition::GetDisassemblyResults");

    if (!CanBeDisassembled || !Checks(Disassemble, CanBeDisassembled) || FirstCall(Results) != CanBeDisassembled)
    {
        Log::Error(L"Finder: item disassembly not found");
        return false;
    }

    if (bBatch)
    {
        Natives::AFortPlayerController_DisassembleInventoryItems = reinterpret_cast<decltype(Natives::AFortPlayerController_DisassembleInventoryItems)>(Disassemble);
    }
    else
    {
        Natives::AFortPlayerController_DisassembleInventoryItem = reinterpret_cast<decltype(Natives::AFortPlayerController_DisassembleInventoryItem)>(Disassemble);
    }
    Natives::UFortWorldItemDefinition_GetDisassemblyResults = reinterpret_cast<decltype(Natives::UFortWorldItemDefinition_GetDisassemblyResults)>(Results);
    Log::Info(L"Finder: AFortPlayerController::DisassembleInventoryItem{} +0x{:X}, UFortWorldItemDefinition::GetDisassemblyResults +0x{:X}", bBatch ? L"s" : L"", Disassemble - Finder::ImageBase(), Results - Finder::ImageBase());
    return true;
}

bool Finder::ResolveLeaveOutpostLive()
{
    if (Natives::AFortGameMode_SendStayWithParty && Natives::AGameMode_ReturnToMainMenuHostIndex >= 0)
    {
        return true;
    }

    const int32_t ReturnIndex = Finder::VTableIndex(CdoVTable(L"/Script/FortniteGame.FortGameMode"), reinterpret_cast<void*>(FunctionOf(L"Ignoring ReturnToMainMenuHost, already in a level travel")), 0x400);
    // The game mode zone's zone end calls it (ZoneEndStayWithParty). The slot is the first of the class's vtable that
    // calls a function with the ClientStayWithParty virtual call, so the zone end finder needn't have run.
    void** ZoneVTable = CdoVTable(L"/Script/FortniteGame.FortGameModeZone");
    const int32_t StaySlot = ClientStayWithPartySlot();
    uintptr_t StayWithParty = 0;
    for (int32_t Slot = 0; ZoneVTable && StaySlot >= 0 && Slot < 0x400 && !StayWithParty; ++Slot)
    {
        StayWithParty = ZoneEndStayWithParty(VTableEntry(ZoneVTable, Slot), StaySlot);
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

    // Slot 0x155 on 1.7.2 (0x7E2860) and 1.8 (0x7FDEC0). It is resolved on the first possess, after the cheat movement
    // hooks have put Core's functions in ACharacter's ClientCheatWalk/Fly/Ghost slots (0x104 to 0x106) of every pawn
    // class: a walk that stops at the first slot outside the game's code never got this far.
    const int32_t Index = FirstVTableIndex(CdoVTable(L"/Script/FortniteGame.FortPlayerPawn"), FunctionOf(L"ResetGameplayAttributes called with valid state"));
    if (Index < 0)
    {
        Log::Error(L"Finder: AFortPlayerPawn::ResetGameplayAttributes slot not found");
        return false;
    }

    Natives::AFortPlayerPawn_ResetGameplayAttributesIndex = Index;
    Log::Info(L"Finder: AFortPlayerPawn::ResetGameplayAttributes is slot 0x{:X}", Index);
    return true;
}

bool Finder::ResolveLootTierRedirectLive()
{
    static bool bSearched = false;
    if (Natives::AFortGameMode_RedirectLootTierGroupIndex >= 0 || bSearched)
    {
        return Natives::AFortGameMode_RedirectLootTierGroupIndex >= 0;
    }
    bSearched = true;

    // AFortGameModeAthena's override of the game mode's loot tier group redirect (1.7.2 0x3FA190, slot 0x156) is the only
    // reader of "Loot_AthenaTreasure": Loot_Treasure -> Loot_AthenaTreasure, Loot_Ammo -> Loot_AthenaAmmoLarge, the Athena
    // floor loot, ammo and supply drop groups unchanged, every other group None. AFortGameMode's (0x212F110) returns its
    // argument, so the slot is found through the Athena override. 3.5 adds a second reader before it, a non-virtual
    // check of the Athena groups (3.5.1 0x9ED730; the override is 0x9F5980): the slot is the first reader's in the vtable.
    void** AthenaVTable = CdoVTable(L"/Script/FortniteGame.FortGameModeAthena");
    int32_t Index = -1;
    for (const uintptr_t Ref : Finder::StringRefs("Loot_AthenaTreasure"))
    {
        Index = FirstVTableIndex(AthenaVTable, Finder::FunctionStart(Ref));
        if (Index >= 0)
        {
            break;
        }
    }
    if (Index < 0)
    {
        Log::Error(L"Finder: AFortGameMode::RedirectLootTierGroup slot not found");
        return false;
    }

    Natives::AFortGameMode_RedirectLootTierGroupIndex = Index;
    Log::Info(L"Finder: AFortGameMode::RedirectLootTierGroup is slot 0x{:X}", Index);
    return true;
}

bool Finder::ResolvePlaylistDataTablesLive()
{
    static bool bSearched = false;
    if (Natives::AFortGameModeAthena_ApplyPlaylistDataTables || bSearched)
    {
        return Natives::AFortGameModeAthena_ApplyPlaylistDataTables != nullptr;
    }
    bSearched = true;

    // AFortGameModeAthena::ApplyPlaylistDataTables (2.4 0x409790, 2.4.2 0x411440, 2.5 0x990C60; AFortGameStateAthena's
    // in 21.40), the game mode's virtual that OnGivenMatchAssignment (2.4.2 0x429330) calls last, logs "Restoring data
    // tables to defaults" (the game instance's AthenaDataTables copied back over AthenaLootTierData_Client,
    // AthenaLootPackages_Client, AthenaRangedWeapons, AthenaGameData and AthenaResourceRates), then copies the tables of
    // the PlaylistDataTables entry whose PlaylistId is CurrentPlaylistId over them (UDataTable::CreateTableFromOtherTable,
    // "Patched Loot Tier with %s"). Builds before 1.11 have no playlist tables. 3.5's (3.5.1 0x9DC890) restores nothing:
    // it logs that it applies the game state's CurrentPlaylistData, then copies that playlist's tables over the defaults.
    uintptr_t Apply = FunctionOf(L"Restoring data tables to defaults");
    if (!Apply)
    {
        Apply = FunctionOf(L"PLAYLIST: Preparing to apply Data Tables and Curves in AFortGameModeAthena::ApplyPlaylistDataTables(), PlaylistName is %s (Server Only)");
    }
    if (!Apply)
    {
        Log::Info(L"Finder: no AFortGameModeAthena::ApplyPlaylistDataTables on this build, the loot tables stay the defaults");
        return false;
    }

    Natives::AFortGameModeAthena_ApplyPlaylistDataTables = reinterpret_cast<decltype(Natives::AFortGameModeAthena_ApplyPlaylistDataTables)>(Apply);
    Log::Info(L"Finder: AFortGameModeAthena::ApplyPlaylistDataTables at 0x{:X}", Apply - Finder::ImageBase());
    return true;
}

bool Finder::ResolveSetCurrentPlaylistIdLive(ptrdiff_t StormReachedFinalPositionOffset)
{
    static bool bSearched = false;
    if (Natives::AFortGameStateAthena_SetCurrentPlaylistId || bSearched)
    {
        return Natives::AFortGameStateAthena_SetCurrentPlaylistId != nullptr;
    }
    bSearched = true;

    // AFortGameStateAthena::SetCurrentPlaylistId (4.1 0xA6DD20, 4.2 0xA592E0) logs the playlist it found in the game
    // mode's playlist manager, then sets CurrentPlaylistData and loads its assets. 4.4 and 4.5 log it from two
    // functions, the id's (4.5 0xA7FF10) and the name's (0xA800A0), which OnGivenMatchAssignment calls; the id's is the
    // one that clears bStormReachedFinalPosition first ("mov byte ptr [rcx + 0x1039], 0"). 5.0 to 5.21 inline both
    // copies into AFortGameModeAthena::OnGivenMatchAssignment (5.21 0xBD59E0, which logs "Assignment Received"), which
    // the old first-reference read took for it (#388: the -CoreBotMatch call crashed 5.21 in AFortGameMode's half),
    // and 5.30 strips them with it: none of its own there, and AFortGameStateAthena::SetCurrentPlaylistId runs its steps.
    const uintptr_t Assignment = FunctionOf(L"PLAYLIST: Assignment Received in AFortGameModeAthena::OnGivenMatchAssignment() PlaylistId is %d (Server-Only)");
    std::vector<uintptr_t> Candidates;
    for (const uintptr_t Reference : Finder::StringRefs(L"PLAYLIST: Playlist Object found in AFortGameStateAthena::SetCurrentPlaylistId() PlaylistName is %s (Server Only)"))
    {
        const uintptr_t Candidate = Finder::FunctionStart(Reference);
        if (Candidate && Candidate != Assignment && std::find(Candidates.begin(), Candidates.end(), Candidate) == Candidates.end())
        {
            Candidates.push_back(Candidate);
        }
    }
    uintptr_t Function = Candidates.size() == 1 ? Candidates.front() : 0;
    if (Candidates.size() > 1 && StormReachedFinalPositionOffset > 0)
    {
        const int32_t Offset = static_cast<int32_t>(StormReachedFinalPositionOffset);
        const auto Bytes = [](int32_t Value, int32_t Index) { return (Value >> (8 * Index)) & 0xFF; };
        for (const uintptr_t Candidate : Candidates)
        {
            if (FindInstruction(Candidate, Candidate + 0x30, { 0xC6, -1, Bytes(Offset, 0), Bytes(Offset, 1), Bytes(Offset, 2), Bytes(Offset, 3), 0x00 }))
            {
                Function = Function ? UINTPTR_MAX : Candidate;
            }
        }
        Function = Function == UINTPTR_MAX ? 0 : Function;
    }
    if (!Function)
    {
        if (Candidates.empty())
        {
            Log::Info(L"Finder: no AFortGameStateAthena::SetCurrentPlaylistId of its own on this build (OnGivenMatchAssignment {}), Core runs its steps",
                Assignment ? L"inlines it" : L"strips it");
        }
        else
        {
            Log::Error(L"Finder: AFortGameStateAthena::SetCurrentPlaylistId not told apart from the name's ({} functions log it), Core runs its steps", Candidates.size());
        }
        return false;
    }
    Natives::AFortGameStateAthena_SetCurrentPlaylistId = reinterpret_cast<decltype(Natives::AFortGameStateAthena_SetCurrentPlaylistId)>(Function);
    Log::Info(L"Finder: AFortGameStateAthena::SetCurrentPlaylistId at 0x{:X}", Function - Finder::ImageBase());
    return true;
}

bool Finder::ResolveMatchAssignmentLive(void** GameModeVTable)
{
    static bool bSearched = false;
    if (bSearched)
    {
        return Natives::AFortGameMode_OnGivenMatchAssignmentIndex >= 0;
    }
    bSearched = true;

    // AFortGameMode::OnGivenMatchAssignment (5.21 0xE0F360, 5.30 0xE31710) reads the assignment's match options: a
    // custom key of more than one character becomes the game state's "CK-%s" session name, else match.option.isPlaytest
    // makes a "Playtest-%s" one.
    const uintptr_t OnGivenMatchAssignment = FunctionOf(L"match.option.isPlaytest");
    void** VTable = OnGivenMatchAssignment ? GameModeVTable : nullptr;
    if (!VTable)
    {
        Log::Info(L"Finder: no AFortGameMode::OnGivenMatchAssignment on this build");
        return false;
    }
    // The first slot holding it: the vtables sit back to back, so the next game mode's repeats it.
    int32_t Index = -1;
    for (int32_t Slot = 0; Slot < 0x200 && Index < 0; ++Slot)
    {
        Index = reinterpret_cast<uintptr_t>(VTable[Slot]) == OnGivenMatchAssignment ? Slot : -1;
    }
    if (Index < 0)
    {
        Log::Error(L"Finder: AFortGameMode::OnGivenMatchAssignment (0x{:X}) is in no slot of AFortGameMode's vtable", OnGivenMatchAssignment - Finder::ImageBase());
        return false;
    }
    Natives::AFortGameMode_OnGivenMatchAssignment = reinterpret_cast<decltype(Natives::AFortGameMode_OnGivenMatchAssignment)>(OnGivenMatchAssignment);
    Natives::AFortGameMode_OnGivenMatchAssignmentIndex = Index;

    // AFortGameMode::SetCurrentPlaylistName (5.30 0xE40340) and AFortGameState::StartStreamingAdditionalPlaylistLevel
    // (5.21 0xE1FE60, 5.30 0xE42750) log these. Only the restored OnGivenMatchAssignment (5.30 on) and
    // AFortGameStateAthena::SetCurrentPlaylistId's steps (5.0 on) use them; builds without playlist assets (before 3.5)
    // or without the name's setter have none.
    const uintptr_t SetCurrentPlaylistName = FunctionOf(L"AFortGameMode::SetCurrentPlaylistName(%s) set playlist to (%s, ID: %d)");
    const uintptr_t StartStreaming = FunctionOf(L"PLAYLIST: Failed to locate valid level to be streamed %s");
    Natives::AFortGameMode_SetCurrentPlaylistName = reinterpret_cast<decltype(Natives::AFortGameMode_SetCurrentPlaylistName)>(SetCurrentPlaylistName);
    Natives::AFortGameState_StartStreamingAdditionalPlaylistLevel = reinterpret_cast<decltype(Natives::AFortGameState_StartStreamingAdditionalPlaylistLevel)>(StartStreaming);
    const auto Where = [](uintptr_t Function) { return Function ? std::format(L"0x{:X}", Function - Finder::ImageBase()) : std::wstring(L"none on this build"); };
    Log::Info(L"Finder: AFortGameMode::OnGivenMatchAssignment is slot 0x{:X} (0x{:X}), SetCurrentPlaylistName {}, StartStreamingAdditionalPlaylistLevel {}",
        Index, OnGivenMatchAssignment - Finder::ImageBase(), Where(SetCurrentPlaylistName), Where(StartStreaming));

    // From 6.01 OnGivenMatchAssignment (CL-4424678 0xE481D0) also takes the party's custom match options: 0xE4C2A0
    // copies the assignment's options and adds each "<key>=<value>" of match.option.customMatchOptions as a pair of its
    // own (unless UFortRuntimeOptions disables matchmaking knobs, or that one), and the game state, when it is an
    // AFortGameState, is given the lot: the next call that is a member's assignment, 0xE9A910 ("add rcx, 5F0h; jmp" to
    // the map's copy; 21.40 names the member MatchOptions). No client build reads the map back.
    const uintptr_t CustomMatchOptions = FunctionOf(L"match.option.customMatchOptions");
    const uintptr_t End = CustomMatchOptions ? Finder::FunctionEnd(OnGivenMatchAssignment) : 0;
    bool bOptionsTaken = false;
    for (const auto& [Site, Target] : End ? Finder::DirectCalls(OnGivenMatchAssignment, End) : std::vector<std::pair<uintptr_t, uintptr_t>>())
    {
        if (Target == CustomMatchOptions)
        {
            bOptionsTaken = true;
        }
        else if (bOptionsTaken && Byte(Target) == 0x48 && Byte(Target + 1) == 0x81 && Byte(Target + 2) == 0xC1 && Byte(Target + 7) == 0xE9)
        {
            const int32_t Offset = *reinterpret_cast<const int32_t*>(Target + 3);
            if (Offset > 0 && Offset < 0x4000)
            {
                Natives::AFortGameState_MatchOptions = Offset;
                Log::Info(L"Finder: AFortGameState::MatchOptions at +0x{:X} (SetMatchOptions 0x{:X}, the custom match options from 0x{:X})", Offset, Target - Finder::ImageBase(),
                    CustomMatchOptions - Finder::ImageBase());
            }
            break;
        }
    }
    if (CustomMatchOptions && Natives::AFortGameState_MatchOptions < 0)
    {
        Log::Error(L"Finder: AFortGameState::MatchOptions not found (the custom match options at 0x{:X} {}), a party's custom match options won't reach its playlist",
            CustomMatchOptions - Finder::ImageBase(), bOptionsTaken ? L"are taken, but the game state isn't given them by a member's assignment" : L"aren't called from OnGivenMatchAssignment");
    }
    return true;
}

bool Finder::ResolveShouldDropItemLive(ptrdiff_t GameplayMutatorsOffset)
{
    static bool bSearched = false;
    if (bSearched)
    {
        return Natives::AFortPlayerController_ShouldDropItemIndex >= 0;
    }
    bSearched = true;

    // AFortPlayerController's virtual that says whether an item drops (6.01 CL-4424678 slot 0x31D; the mutators' side of
    // it is 21.40's FortGameStateAthena_ShouldDropItem): (the item's definition, the answer otherwise) and returns the
    // answer. AFortPlayerController's and Zone's return what they were given ("movzx eax, r8b; ret", 0xC58580, in no
    // other slot of the vtable); AFortPlayerControllerAthena's (0xC26970) asks each active mutator of the game state's
    // GameplayMutators ("mov rbx, [rsi+1AF0h]") through its own virtual (slot 0xD7) and stops at the first that
    // overrides. Its caller, the death's item drop, is stripped, so nothing in the build calls it.
    void** ControllerVTable = CdoVTable(L"/Script/FortniteGame.FortPlayerController");
    void** AthenaVTable = CdoVTable(L"/Script/FortniteGame.FortPlayerControllerAthena");
    const int32_t Mutators = static_cast<int32_t>(GameplayMutatorsOffset);
    int32_t Index = -1;
    uintptr_t Function = 0;
    for (int32_t Slot = 0; ControllerVTable && AthenaVTable && Mutators > 0 && Slot < 0x400 && Index < 0; ++Slot)
    {
        const uintptr_t Base = reinterpret_cast<uintptr_t>(ControllerVTable[Slot]);
        const uintptr_t Athena = reinterpret_cast<uintptr_t>(AthenaVTable[Slot]);
        // The vtables' ends; a slot Core has swapped holds one of its own functions.
        if ((!IsInText(Base) && !IsCoreFunction(Base)) || (!IsInText(Athena) && !IsCoreFunction(Athena)))
        {
            break;
        }
        if (!IsInText(Base) || !IsInText(Athena) || Athena == Base || !Finder::BodyIs(Base, { 0x41, 0x0F, 0xB6, 0xC0, 0xC3 }))
        {
            continue;
        }
        const uintptr_t AthenaEnd = Finder::FunctionStart(Athena) == Athena ? Finder::FunctionEnd(Athena) : 0;
        // Whether a function of up to 0x200 bytes loads GameplayMutators and calls a virtual: the loop over the mutators.
        const auto AsksMutators = [Mutators](uintptr_t Begin, uintptr_t End)
        {
            bool bReadsMutators = false;
            for (uintptr_t Load = End > Begin && End - Begin <= 0x200 ? Begin : 0; Load && Load + 7 <= End && !bReadsMutators; ++Load)
            {
                bReadsMutators = (Byte(Load) & 0xF8) == 0x48 && Byte(Load + 1) == 0x8B && (Byte(Load + 2) & 0xC0) == 0x80 && (Byte(Load + 2) & 0x07) != 0x04
                    && *reinterpret_cast<const int32_t*>(Load + 3) == Mutators;
            }
            return bReadsMutators && FindVirtualCall(Begin, End) != 0;
        };
        bool bAsksMutators = AsksMutators(Athena, AthenaEnd);
        // 6.10 (CL-4480234) keeps the loop in the game state's own function (0xC2F9E0: mov rbx, [rcx+1B10h], each
        // mutator's slot 0xD6), which Athena's override (0xC677E0) calls with the answer so far, the controller and the
        // item.
        for (const auto& [Call, Target] : !bAsksMutators && AthenaEnd > Athena && AthenaEnd - Athena <= 0x200 ? Finder::DirectCalls(Athena, AthenaEnd) : decltype(Finder::DirectCalls(0, 0)){})
        {
            if (IsInText(Target) && Finder::FunctionStart(Target) == Target && AsksMutators(Target, Finder::FunctionEnd(Target)))
            {
                bAsksMutators = true;
                break;
            }
        }
        if (bAsksMutators)
        {
            Index = Slot;
            Function = Athena;
        }
    }
    if (Index < 0)
    {
        Log::Error(L"Finder: AFortPlayerController::ShouldDropItem not found (GameplayMutators +0x{:X}), the game state's mutators aren't asked which items an eliminated player drops",
            GameplayMutatorsOffset > 0 ? static_cast<int32_t>(GameplayMutatorsOffset) : 0);
        return false;
    }
    Natives::AFortPlayerController_ShouldDropItemIndex = Index;
    Log::Info(L"Finder: AFortPlayerController::ShouldDropItem is slot 0x{:X} (Athena's 0x{:X} asks the game state's mutators)", Index, Function - Finder::ImageBase());
    return true;
}

bool Finder::ResolveIsRespawningAllowedLive(ptrdiff_t GameplayMutatorsOffset, ptrdiff_t AllowRespawningOffset, uint8_t AllowRespawningMask)
{
    static bool bSearched = false;
    if (bSearched)
    {
        return Natives::AFortGameModeAthena_IsRespawningAllowedIndex >= 0;
    }
    bSearched = true;

    // AFortGameModeAthena::IsRespawningAllowed(AFortPlayerStateAthena*) (6.01 CL-4424678 0xBCF370, 6.02 CL-4461277
    // 0xBD08A0, slot 0x135 in both; the mutators' side of it is 21.40's FortGameStateAthena_IsRespawningAllowed). It
    // asks each of the game mode's GameplayMutators ("mov rbx, [rcx+0F10h]", their slot 0xC0) and returns the answer of
    // the first that has one; else false unless the game state's bAllowRespawning is set ("test byte ptr [rbx+1B00h],
    // 1"); else true, unless the playlist is InfiniteRespawnExceptStorm and the player died outside the safe zone. Other
    // game mode functions read the mutators (Tick, PlayerCanRestart, FinishRestartPlayer), none of them tests the flag.
    void** VTable = CdoVTable(L"/Script/FortniteGame.FortGameModeAthena");
    const int32_t Mutators = static_cast<int32_t>(GameplayMutatorsOffset);
    const int32_t AllowRespawning = static_cast<int32_t>(AllowRespawningOffset);
    int32_t Index = -1;
    int32_t Count = 0;
    uintptr_t Function = 0;
    for (int32_t Slot = 0; VTable && Mutators > 0 && AllowRespawning > 0 && AllowRespawningMask && Slot < 0x400; ++Slot)
    {
        const uintptr_t Entry = reinterpret_cast<uintptr_t>(VTable[Slot]);
        // The vtable's end; a slot Core has swapped holds one of its own functions.
        if (!IsInText(Entry) && !IsCoreFunction(Entry))
        {
            break;
        }
        const uintptr_t End = IsInText(Entry) && Finder::FunctionStart(Entry) == Entry ? Finder::FunctionEnd(Entry) : 0;
        if (End <= Entry || End - Entry > 0x400)
        {
            continue;
        }
        bool bReadsMutators = false;
        bool bTestsAllowRespawning = false;
        for (uintptr_t Address = Entry; Address + 7 <= End; ++Address)
        {
            // mov r64, [reg+GameplayMutators]
            bReadsMutators = bReadsMutators || ((Byte(Address) & 0xF8) == 0x48 && Byte(Address + 1) == 0x8B && (Byte(Address + 2) & 0xC0) == 0x80 && (Byte(Address + 2) & 0x07) != 0x04
                && *reinterpret_cast<const int32_t*>(Address + 3) == Mutators);
            // test byte ptr [reg+bAllowRespawning], mask
            bTestsAllowRespawning = bTestsAllowRespawning || (Byte(Address) == 0xF6 && (Byte(Address + 1) & 0xF8) == 0x80 && (Byte(Address + 1) & 0x07) != 0x04
                && *reinterpret_cast<const int32_t*>(Address + 2) == AllowRespawning && Byte(Address + 6) == AllowRespawningMask);
        }
        // The first slot holding it: the vtables sit back to back, so a subclass's that follows repeats it.
        if (bReadsMutators && bTestsAllowRespawning && FindVirtualCall(Entry, End) && Entry != Function)
        {
            Index = Count == 0 ? Slot : Index;
            Function = Count == 0 ? Entry : Function;
            ++Count;
        }
    }
    if (Count != 1)
    {
        Log::Error(L"Finder: AFortGameModeAthena::IsRespawningAllowed not found ({} functions read GameplayMutators +0x{:X} and test bAllowRespawning +0x{:X}), a death asks the playlist's RespawnType only",
            Count, Mutators > 0 ? Mutators : 0, AllowRespawning > 0 ? AllowRespawning : 0);
        return false;
    }
    Natives::AFortGameModeAthena_IsRespawningAllowedIndex = Index;
    Log::Info(L"Finder: AFortGameModeAthena::IsRespawningAllowed is slot 0x{:X} (0x{:X} asks the game mode's mutators, then the game state's bAllowRespawning)", Index, Function - Finder::ImageBase());
    return true;
}

bool Finder::ResolveLootDataTablesLive(ptrdiff_t GameDataLootTierDataTablesOffset, ptrdiff_t PlaylistLootTierDataOffset, ptrdiff_t PlaylistLootPackagesOffset)
{
    static bool bSearched = false;
    if (bSearched)
    {
        return Natives::AFortGameMode_GetLootTierDataTablesIndex >= 0 && Natives::AFortGameMode_GetLootPackageDataTablesIndex >= 0;
    }
    bSearched = true;

    // AFortGameMode::GetLootTierDataTables and GetLootPackageDataTables(TArray<UDataTable*>&) (21.40's names), the tables
    // the server's loot rolls read. The game mode's own (6.02 CL-4461277 0xE43050, slot 0x132) loads the game data's
    // LootTierDataTables ("lea rcx, [rax+0E98h]" after UFortGlobals' game data), its packages one (0xE43030, slot 0x133)
    // the LootPackageDataTables. AFortGameModeAthena's (0xBCB900 and 0xBCB830) load the game state's playlist's
    // LootTierData ("lea rcx, [rax+170h]") and LootPackages ("lea rcx, [rax+198h]"), and call their Super when it has
    // none. From 5.21 that is how a playlist's loot reaches the rolls: up to 5.10 ApplyPlaylistDataTables copies the
    // playlist's rows into the default tables ("Patched Loot Tier with %s"), 5.21's and later's patch only the ranged
    // weapons, game data and resource rates (6.02 0xBB59D0).
    void** GameModeVTable = CdoVTable(L"/Script/FortniteGame.FortGameMode");
    void** AthenaVTable = CdoVTable(L"/Script/FortniteGame.FortGameModeAthena");
    const auto SlotCount = [](void** VTable)
    {
        int32_t Count = 0;
        while (VTable && Count < 0x400 && (IsInText(reinterpret_cast<uintptr_t>(VTable[Count])) || IsCoreFunction(reinterpret_cast<uintptr_t>(VTable[Count]))))
        {
            ++Count;
        }
        return Count;
    };
    // A function's first piece, when Entry starts one no bigger than MaxSize. 6.20 (CL-4497486) splits AFortGameModeAthena's
    // overrides into pieces whose first holds only the prologue (0xC726B0: C726B0-C726BA, then C726BA-C7272A,
    // C7272A-C72763 and C7276E-C72784, each chained to the first), so with a first piece that short the body runs to its
    // last piece's end.
    const auto BodyEnd = [](uintptr_t Entry, uintptr_t MaxSize) -> uintptr_t
    {
        uintptr_t End = IsInText(Entry) && Finder::FunctionStart(Entry) == Entry ? Finder::FunctionEnd(Entry) : 0;
        if (End > Entry && End - Entry < 0x20)
        {
            for (const auto& [ChunkBegin, ChunkEnd] : Finder::FunctionChunks(Entry))
            {
                End = ChunkBegin >= Entry && ChunkEnd - Entry <= MaxSize ? std::max(End, ChunkEnd) : End;
            }
        }
        return End > Entry && End - Entry <= MaxSize ? End : 0;
    };
    // lea r64, [reg+Offset], or 6.20's add r64, Offset (add rcx, 198h at 0xC72723, the playlist's LootTierData).
    const auto LoadsAddressAt = [](uintptr_t Begin, uintptr_t End, int32_t Offset)
    {
        for (uintptr_t Address = Begin; Offset > 0 && Address + 7 <= End; ++Address)
        {
            if ((Byte(Address) & 0xFA) == 0x48 && Byte(Address + 1) == 0x8D && (Byte(Address + 2) & 0xC0) == 0x80 && (Byte(Address + 2) & 0x07) != 0x04
                && *reinterpret_cast<const int32_t*>(Address + 3) == Offset)
            {
                return true;
            }
            // Not add rsp, imm32: a stack frame's size can equal the offset.
            if ((Byte(Address) & 0xFE) == 0x48 && Byte(Address + 1) == 0x81 && (Byte(Address + 2) & 0xF8) == 0xC0 && Byte(Address + 2) != 0xC4
                && *reinterpret_cast<const int32_t*>(Address + 3) == Offset)
            {
                return true;
            }
        }
        return false;
    };
    // call or jmp Target
    const auto CallsDirectly = [](uintptr_t Begin, uintptr_t End, uintptr_t Target)
    {
        for (uintptr_t Address = Begin; Address + 5 <= End; ++Address)
        {
            if ((Byte(Address) == 0xE8 || Byte(Address) == 0xE9) && Finder::RelativeTarget(Address, 1, 5) == Target)
            {
                return true;
            }
        }
        return false;
    };

    const int32_t Slots = std::min(SlotCount(GameModeVTable), SlotCount(AthenaVTable));
    int32_t TierIndex = -1;
    int32_t PackageIndex = -1;
    int32_t TierCount = 0;
    int32_t PackageCount = 0;
    uintptr_t TierFunction = 0;
    uintptr_t PackageFunction = 0;
    for (int32_t Slot = 0; Slot < Slots; ++Slot)
    {
        const uintptr_t Athena = reinterpret_cast<uintptr_t>(AthenaVTable[Slot]);
        const uintptr_t Super = reinterpret_cast<uintptr_t>(GameModeVTable[Slot]);
        const uintptr_t AthenaEnd = Athena != Super ? BodyEnd(Athena, 0x200) : 0;
        if (!AthenaEnd || !CallsDirectly(Athena, AthenaEnd, Super))
        {
            continue;
        }
        const uintptr_t SuperEnd = BodyEnd(Super, 0x80);
        if (SuperEnd && LoadsAddressAt(Athena, AthenaEnd, static_cast<int32_t>(PlaylistLootTierDataOffset))
            && LoadsAddressAt(Super, SuperEnd, static_cast<int32_t>(GameDataLootTierDataTablesOffset)) && Athena != TierFunction)
        {
            TierIndex = TierCount == 0 ? Slot : TierIndex;
            TierFunction = TierCount == 0 ? Athena : TierFunction;
            ++TierCount;
        }
        if (LoadsAddressAt(Athena, AthenaEnd, static_cast<int32_t>(PlaylistLootPackagesOffset)) && Athena != PackageFunction)
        {
            PackageIndex = PackageCount == 0 ? Slot : PackageIndex;
            PackageFunction = PackageCount == 0 ? Athena : PackageFunction;
            ++PackageCount;
        }
    }
    if (TierCount != 1 || PackageCount != 1)
    {
        // Up to 5.10 the default tables hold the playlist's rows, so the game data's are the match's.
        if (!First(Finder::StringRefs(L"PLAYLIST: Patched Loot Tier with %s")) && !First(Finder::StringRefs(L"Patched Loot Tier with %s")))
        {
            Log::Error(L"Finder: AFortGameMode::GetLootTierDataTables not found ({} Athena overrides load the playlist's LootTierData +0x{:X}, {} its LootPackages +0x{:X}), the loot rolls read the default tables",
                TierCount, PlaylistLootTierDataOffset, PackageCount, PlaylistLootPackagesOffset);
        }
        else
        {
            Log::Info(L"Finder: no AFortGameModeAthena::GetLootTierDataTables, ApplyPlaylistDataTables copies the playlist's loot rows into the default tables");
        }
        return false;
    }
    Natives::AFortGameMode_GetLootTierDataTablesIndex = TierIndex;
    Natives::AFortGameMode_GetLootPackageDataTablesIndex = PackageIndex;
    Log::Info(L"Finder: AFortGameMode::GetLootTierDataTables is slot 0x{:X}, GetLootPackageDataTables slot 0x{:X} (AFortGameModeAthena's 0x{:X} and 0x{:X} give the playlist's LootTierData and LootPackages)",
        TierIndex, PackageIndex, TierFunction - Finder::ImageBase(), PackageFunction - Finder::ImageBase());
    return true;
}

bool Finder::ResolvePersistentGameplayEffectsLive()
{
    static bool bSearched = false;
    if (bSearched)
    {
        return Natives::AFortGameStateZone_OnAbilitySystemInterfaceReadyForPersistentGameplayEffects && Natives::UObject_GetInterfaceAddress;
    }
    bSearched = true;

    // AFortGameStateZone::OnAbilitySystemInterfaceReadyForPersistentGameplayEffects(IAbilitySystemInterface*) (21.40's
    // name and its stat: 4.2 0xC3D5E0, 5.21 0xE5FC60, 5.41 0xE81110, 6.00 0xEC4B30) is the only function that opens a
    // STAT_Fort_GameStateZoneApplyingPersistentEffects scope: on the authority it spawns the game state's
    // GlobalEnvironmentAbilityActor if it has none, then has each of ActiveGameplayModifiers apply its modifier's
    // persistent effects and ability sets to the actor (FActiveGameplayModifier::ApplyModifierToAbilitySystemInterface,
    // 5.41 0xE6AC60). 1.x builds compile their stats out, so they have no name to find it by.
    const std::vector<uintptr_t> Refs = Finder::StringRefs(L"STAT_Fort_GameStateZoneApplyingPersistentEffects");
    const uintptr_t Ready = Finder::FunctionStart(First(Refs));
    if (!IsInText(Ready))
    {
        Log::Info(L"Finder: AFortGameStateZone::OnAbilitySystemInterfaceReadyForPersistentGameplayEffects not found ({} references to its stat), so no actor gets the gameplay modifiers' persistent effects from Core", Refs.size());
        return false;
    }
    for (const uintptr_t Ref : Refs)
    {
        if (Finder::FunctionStart(Ref) != Ready)
        {
            Log::Error(L"Finder: STAT_Fort_GameStateZoneApplyingPersistentEffects is in more than one function (0x{:X}, 0x{:X})", Ready - Finder::ImageBase(), Finder::FunctionStart(Ref) - Finder::ImageBase());
            return false;
        }
    }

    if (!Natives::UObject_GetInterfaceAddress)
    {
        ResolveSpottingLive(FindObject<UClass>(nullptr, L"/Script/FortniteGame.FortSpottableActorInterface"));
    }
    if (!Natives::UObject_GetInterfaceAddress)
    {
        Log::Error(L"Finder: UObject::GetInterfaceAddress not found, so no actor's IAbilitySystemInterface for AFortGameStateZone::OnAbilitySystemInterfaceReadyForPersistentGameplayEffects");
        return false;
    }
    Natives::AFortGameStateZone_OnAbilitySystemInterfaceReadyForPersistentGameplayEffects = reinterpret_cast<decltype(Natives::AFortGameStateZone_OnAbilitySystemInterfaceReadyForPersistentGameplayEffects)>(Ready);
    Log::Info(L"Finder: AFortGameStateZone::OnAbilitySystemInterfaceReadyForPersistentGameplayEffects at 0x{:X}", Ready - Finder::ImageBase());
    return true;
}

bool Finder::ResolveFlightPathLive(ptrdiff_t FlightPathInitializedOffset)
{
    static bool bSearched = false;
    if (Natives::AFortAthenaMapInfo_InitializeFlightPath || bSearched)
    {
        return Natives::AFortAthenaMapInfo_InitializeFlightPath != nullptr;
    }
    if (FlightPathInitializedOffset <= 0 || !Natives::AFortGameModeAthena_StartAircraftPhase)
    {
        return false;
    }
    bSearched = true;

    // The aircraft paths are built once, while AFortGameModeAthena's bFlightPathInitialized is false, the same way in
    // each place that builds them:
    //   cmp byte ptr [GameMode + bFlightPathInitialized], 0; jnz; mov rcx, MapInfo; call IsMapInfoValidForAircraft;
    //   test al, al; jz; mov rdx, GameState; mov rcx, MapInfo; call InitializeFlightPath;
    //   mov byte ptr [GameMode + bFlightPathInitialized], 1
    // 3.5.1 to 4.1 have it in OnPlaylistDataLoaded (4.1 0xA2F500), StartWarmupPhase (0xA3D1B0) and StartAircraftPhase
    // (0xA3B530), so a match that never loads its playlist (the bots-only test match) still has its paths when the bus
    // leaves. 4.2 keeps only OnPlaylistDataLoaded's (0xA1BE40), and its StartAircraftPhase (0xA28610) spawns the
    // aircraft (0xA27910) on FlightInfos[index] with no check that a path is there. Core only needs the build where
    // StartAircraftPhase no longer has it.
    const int32_t Offset = static_cast<int32_t>(FlightPathInitializedOffset);
    const auto Bytes = [](int32_t Value, int32_t Index) { return (Value >> (8 * Index)) & 0xFF; };
    const std::initializer_list<int32_t> Check = { 0x80, -1, Bytes(Offset, 0), Bytes(Offset, 1), Bytes(Offset, 2), Bytes(Offset, 3), 0x00, 0x75 };
    const uintptr_t AircraftPhase = reinterpret_cast<uintptr_t>(Natives::AFortGameModeAthena_StartAircraftPhase);
    if (FindInstruction(AircraftPhase, Finder::FunctionEnd(AircraftPhase), Check))
    {
        Log::Info(L"Finder: AFortGameModeAthena::StartAircraftPhase builds the aircraft paths itself on this build");
        return false;
    }

    const uintptr_t Loaded = FunctionOf(L"PLAYLIST: Playlist Object finished loading its assets and GameMode can now use the assets AFortGameModeAthena::OnPlaylistDataLoaded(), PlaylistName is %s (Server Only)");
    const uintptr_t LoadedEnd = Loaded ? Finder::FunctionEnd(Loaded) : 0;
    uintptr_t IsValid = 0;
    uintptr_t Initialize = 0;
    for (uintptr_t Site = FindInstruction(Loaded, LoadedEnd, Check); Site && !Initialize; Site = FindInstruction(Site + 1, LoadedEnd, Check))
    {
        // jnz rel8; mov rcx, MapInfo; call IsMapInfoValidForAircraft; test al, al; jz rel8; mov rdx, ...; mov rcx, ...; call
        const uintptr_t ValidCall = FindInstruction(Site + 9, Site + 14, { 0x8B, -1, 0xE8 }) + 2;
        if (ValidCall == 2 || Byte(ValidCall + 5) != 0x84 || Byte(ValidCall + 6) != 0xC0 || Byte(ValidCall + 7) != 0x74)
        {
            continue;
        }
        const uintptr_t InitCall = FindInstruction(ValidCall + 9, ValidCall + 18, { 0x8B, -1, -1, 0x8B, -1, 0xE8 }) + 5;
        if (InitCall == 5 || !FindInstruction(InitCall + 5, InitCall + 13, { 0xC6, -1, Bytes(Offset, 0), Bytes(Offset, 1), Bytes(Offset, 2), Bytes(Offset, 3), 0x01 }))
        {
            continue;
        }
        IsValid = Finder::RelativeTarget(ValidCall, 1, 5);
        Initialize = Finder::RelativeTarget(InitCall, 1, 5);
    }
    if (!IsInText(IsValid) || !IsInText(Initialize))
    {
        Log::Error(L"Finder: AFortAthenaMapInfo::InitializeFlightPath not found (OnPlaylistDataLoaded {}), StartAircraftPhase doesn't build the aircraft paths", Loaded != 0);
        return false;
    }
    Natives::AFortAthenaMapInfo_IsMapInfoValidForAircraft = reinterpret_cast<decltype(Natives::AFortAthenaMapInfo_IsMapInfoValidForAircraft)>(IsValid);
    Natives::AFortAthenaMapInfo_InitializeFlightPath = reinterpret_cast<decltype(Natives::AFortAthenaMapInfo_InitializeFlightPath)>(Initialize);
    Log::Info(L"Finder: AFortAthenaMapInfo::InitializeFlightPath at 0x{:X}, IsMapInfoValidForAircraft at 0x{:X} (OnPlaylistDataLoaded 0x{:X}); StartAircraftPhase doesn't build the aircraft paths",
        Initialize - Finder::ImageBase(), IsValid - Finder::ImageBase(), Loaded - Finder::ImageBase());
    return true;
}

bool Finder::ResolvePoiVolumeLive()
{
    static bool bSearched = false;
    if (Natives::FBodyInstance_InitBody || bSearched)
    {
        return Natives::FBodyInstance_InitBody != nullptr;
    }
    bSearched = true;

    // AFortPoiVolume::PostInitializeComponents (3.0 0x9FDF20; the POI manager is new in 3.0) makes the volume's overlap
    // body once its brush component is there: it logs "Setting up PoiCollisionComp for %s", reads the brush component's
    // body setup (its virtual GetBodySetup, call [rax+628h]), then builds FInitBodySpawnParams from PoiCollisionComp
    // (0x21F5570) and calls FBodyInstance::InitBody (0x2206250) on PoiCollisionComp->PoiBodyInstance with the
    // component's ComponentToWorld and no physics scene: the two direct calls after the virtual one. 3.5 moves the two
    // calls into a helper of their own (3.5.1 0xA5E220) that the volume tail-jumps to with the component and the body
    // setup, so the calls are traced through that jump in the order they run.
    const uintptr_t Ref = First(Finder::StringRefs(L"AFortPoiVolume::PostInitializeComponents - Setting up PoiCollisionComp for %s"));
    if (!Ref)
    {
        Log::Info(L"Finder: no AFortPoiVolume on this build");
        return false;
    }
    const auto IsDirectCall = [](uintptr_t Instruction) { return Byte(Instruction) == 0xE8; };
    const uintptr_t GetBodySetupCall = Finder::TraceFind(Ref, 48, [](uintptr_t Instruction) { return Byte(Instruction) == 0xFF && Byte(Instruction + 1) == 0x90; });
    const uintptr_t ParamsCall = GetBodySetupCall ? Finder::TraceFind(GetBodySetupCall + 6, 24, IsDirectCall) : 0;
    const uintptr_t InitBodyCall = ParamsCall ? Finder::TraceFind(ParamsCall + 5, 24, IsDirectCall) : 0;
    const uintptr_t Params = ParamsCall ? Finder::RelativeTarget(ParamsCall, 1, 5) : 0;
    const uintptr_t InitBody = InitBodyCall ? Finder::RelativeTarget(InitBodyCall, 1, 5) : 0;
    if (!IsInText(Params) || !IsInText(InitBody))
    {
        Log::Error(L"Finder: FBodyInstance::InitBody not found after AFortPoiVolume::PostInitializeComponents' log (0x{:X}), POI volumes stay shapeless", Ref - Finder::ImageBase());
        return false;
    }
    Natives::FInitBodySpawnParams_Construct = reinterpret_cast<decltype(Natives::FInitBodySpawnParams_Construct)>(Params);
    Natives::FBodyInstance_InitBody = reinterpret_cast<decltype(Natives::FBodyInstance_InitBody)>(InitBody);
    Log::Info(L"Finder: FInitBodySpawnParams::FInitBodySpawnParams at 0x{:X}, FBodyInstance::InitBody at 0x{:X} (from AFortPoiVolume::PostInitializeComponents)",
        Params - Finder::ImageBase(), InitBody - Finder::ImageBase());
    return true;
}

bool Finder::ResolveSupplyDropSpawnLive(ptrdiff_t LlamaClassOffset)
{
    static bool bSearched = false;
    if ((Natives::AFortAthenaMapInfo_PickSupplyDropLocation && Natives::AFortAthenaMapInfo_SpawnSupplyDropAtLocation) || bSearched)
    {
        return Natives::AFortAthenaMapInfo_PickSupplyDropLocation && Natives::AFortAthenaMapInfo_SpawnSupplyDropAtLocation;
    }
    bSearched = true;

    // AFortAthenaMapInfo::PickSupplyDropLocation(Center, Radius) (3.3 0x9F8E80) tries 1000 random points in the circle
    // (at Center's height) for one inside AircraftDropVolume, and logs this when none is. AFortGameModeAthena's console
    // commands (3.3 0x9E8230) place a Supply Llama in front of each player for "SPAWNLLAMA": the map info's LlamaClass
    // (mov r9, [r14+3C8h]) is the class argument of AFortAthenaMapInfo::SpawnSupplyDropAtLocation(Location, Rotation,
    // Class) (0xA03800), the call after it, which spawns the drop, moves it onto the ground under Location
    // (AFortAthenaSupplyDrop::FindGroundLocationAt) and finishes it. Neither is in a build before 3.3's Supply Llama.
    const uintptr_t Pick = FunctionOf(L"PickSupplyDropLocation: Failed to find valid location using rejection.  Using safe zone location.");
    const uintptr_t Command = First(Finder::StringRefs(L"SPAWNLLAMA"));
    if (!Pick || !Command || LlamaClassOffset <= 0)
    {
        Log::Info(L"Finder: no Supply Llamas on this build");
        return false;
    }
    uintptr_t ClassLoad = 0;
    for (uintptr_t Load = FindInstruction(Command, Command + 0x300, { -1, 0x8B, -1 }); Load && !ClassLoad; Load = FindInstruction(Load + 1, Command + 0x300, { -1, 0x8B, -1 }))
    {
        // mov r9, [reg+disp32]: REX.WR (4C, or 4D for r8 to r15), ModRM 10 001 xxx.
        const uint8_t Rex = Byte(Load);
        const uint8_t ModRM = Byte(Load + 2);
        if ((Rex == 0x4C || Rex == 0x4D) && (ModRM & 0xF8) == 0x88 && (ModRM & 7) != 4 && *reinterpret_cast<const int32_t*>(Load + 3) == LlamaClassOffset)
        {
            ClassLoad = Load;
        }
    }
    const uintptr_t SpawnCall = ClassLoad ? FindRawCall(ClassLoad + 7, ClassLoad + 0x40) : 0;
    const uintptr_t Spawn = SpawnCall ? Finder::RelativeTarget(SpawnCall, 1, 5) : 0;
    if (!Spawn || Finder::FunctionStart(Spawn) != Spawn)
    {
        Log::Error(L"Finder: AFortAthenaMapInfo::SpawnSupplyDropAtLocation not found after \"SPAWNLLAMA\" (0x{:X}), no Supply Llamas", Command - Finder::ImageBase());
        return false;
    }

    // From 3.6 SpawnSupplyDropAtLocation takes two more floats, the heights its ground trace starts and ends at, and the
    // command passes the map info's SupplyDropMaxPlacementHeight and SupplyDropMinPlacementHeight on the stack (3.6
    // 0x9EE8A2/0x9EE8B0, 4.2 0xA10C3A/0xA10C48: movss [rsp+28h], xmm0; movss [rsp+20h], xmm6) just before the call. 3.5's
    // (0x9E6893) passes four arguments.
    const auto StoresStackFloat = [SpawnCall](int32_t Displacement)
    {
        for (uintptr_t Store = FindInstruction(SpawnCall - 0x30, SpawnCall, { 0xF3, 0x0F, 0x11, -1, 0x24, Displacement }); Store;
             Store = FindInstruction(Store + 1, SpawnCall, { 0xF3, 0x0F, 0x11, -1, 0x24, Displacement }))
        {
            if ((Byte(Store + 3) & 0xC7) == 0x44)
            {
                return true;
            }
        }
        return false;
    };
    Natives::AFortAthenaMapInfo_SpawnSupplyDropAtLocationTakesHeights = StoresStackFloat(0x20) && StoresStackFloat(0x28);

    Natives::AFortAthenaMapInfo_PickSupplyDropLocation = reinterpret_cast<decltype(Natives::AFortAthenaMapInfo_PickSupplyDropLocation)>(Pick);
    Natives::AFortAthenaMapInfo_SpawnSupplyDropAtLocation = reinterpret_cast<decltype(Natives::AFortAthenaMapInfo_SpawnSupplyDropAtLocation)>(Spawn);
    Log::Info(L"Finder: AFortAthenaMapInfo::PickSupplyDropLocation at 0x{:X}, AFortAthenaMapInfo::SpawnSupplyDropAtLocation at 0x{:X}{}", Pick - Finder::ImageBase(), Spawn - Finder::ImageBase(),
        Natives::AFortAthenaMapInfo_SpawnSupplyDropAtLocationTakesHeights ? L" (takes the trace heights)" : L"");
    return true;
}

uint8_t* Finder::AircraftPhasePlayerCleanupCall(ptrdiff_t QuickBarsOffset)
{
    // AFortGameModeAthena::StartAircraftPhase (1.7.2 0x3FFB60) binds the aircraft's delegates by name; OnAircraftFlightEnded
    // (0x3F37D0) unbinds them with copies of the same strings and calls nothing stripped. For each player the phase start
    // unequips the pawn's weapon (0x783330), calls a stripped function (0x3FFFA7: the controller, 0, 0, 0.0f, true),
    // destroys the pawn, calls another with only the controller (0x3FFFC1), then UnPossess and ChangeState(Inactive): two
    // calls to the shared empty function, the second the one wanted. 1.8.1 (0x437880) and 1.8.2 (0x438B40) dropped the
    // controller-only call: only the (controller, 0, 0, 0.0f, true) one is left (1.8.1 0x437CD1, 1.8.2 0x438FA1, with
    // xorps xmm3, xmm3 before it), just before the warmup pawn is destroyed. It gets the controller in rcx too, so it takes
    // the cleanup when no function has the pair.
    // That one is AFortPlayerController::ActivateSlot(Primary, 0, 0.0f, true), the harvesting tool's slot: 2.2 (0x42A960)
    // compiles it in (0x42ADA1 calls 0x85DCA0: if QuickBars (mov rcx, [rcx+1B60h]), its ServerActivateSlotInternal), so
    // there it is no stub and the cleanup went with it (#190). Nothing else in 2.2's aircraft phase removes the warmup
    // island's items: OnAircraftExitedDropZone (0x41EF30) only restarts or kicks the players. That call takes the cleanup
    // too, and the slot is still activated after it (Natives::AFortPlayerController_ActivateSlot). 3.6 (0xEDD160) still
    // tail-jumps to ServerActivateSlotInternal; 4.0's (0xF3F800) adds its bForceExecution (false) and calls it from a
    // frame of its own (sub rsp, 38h first), so the QuickBars load comes after that.
    std::vector<uintptr_t> LoneStubCalls;
    std::vector<std::pair<uintptr_t, uintptr_t>> ActivateSlotCalls;
    const auto IsActivateSlot = [QuickBarsOffset](uintptr_t Target)
    {
        const uintptr_t Load = IsInText(Target) && Byte(Target) == 0x48 && Byte(Target + 1) == 0x83 && Byte(Target + 2) == 0xEC ? Target + 4 : Target;
        return QuickBarsOffset > 0 && IsInText(Target) && Byte(Load) == 0x48 && Byte(Load + 1) == 0x8B && Byte(Load + 2) == 0x89
            && *reinterpret_cast<const int32_t*>(Load + 3) == QuickBarsOffset;
    };
    for (const uintptr_t Ref : Finder::StringRefs(L"&AFortGameModeAthena::OnAircraftEnteredDropZone"))
    {
        const uintptr_t Function = Finder::FunctionStart(Ref);
        const uintptr_t End = Function ? Finder::FunctionEnd(Function) : 0;
        std::vector<uintptr_t> StubCalls;
        for (const auto& [Site, Target] : End ? Finder::DirectCalls(Ref, End) : std::vector<std::pair<uintptr_t, uintptr_t>>())
        {
            if (reinterpret_cast<void*>(Target) == Natives::SharedEmptyFunction)
            {
                StubCalls.push_back(Site);
            }
            else if (IsActivateSlot(Target) && FindInstruction(Site - 0x14, Site, { 0x0F, 0x57, 0xDB }))
            {
                ActivateSlotCalls.emplace_back(Site, Target);
            }
        }
        if (StubCalls.size() == 2)
        {
            Natives::AFortGameModeAthena_StartAircraftPhase = reinterpret_cast<decltype(Natives::AFortGameModeAthena_StartAircraftPhase)>(Function);
            Log::Info(L"Finder: AFortGameModeAthena::StartAircraftPhase at 0x{:X} clears each player at 0x{:X}", Function - Finder::ImageBase(), StubCalls[1] - Finder::ImageBase());
            return reinterpret_cast<uint8_t*>(StubCalls[1]);
        }
        if (StubCalls.size() == 1 && FindInstruction(StubCalls[0] - 0x14, StubCalls[0], { 0x0F, 0x57, 0xDB }))
        {
            LoneStubCalls.push_back(StubCalls[0]);
        }
    }
    if (LoneStubCalls.size() == 1)
    {
        Natives::AFortGameModeAthena_StartAircraftPhase = reinterpret_cast<decltype(Natives::AFortGameModeAthena_StartAircraftPhase)>(Finder::FunctionStart(LoneStubCalls[0]));
        Log::Info(L"Finder: AFortGameModeAthena::StartAircraftPhase at 0x{:X} clears each player at 0x{:X}, before the warmup pawn is destroyed", Finder::FunctionStart(LoneStubCalls[0]) - Finder::ImageBase(), LoneStubCalls[0] - Finder::ImageBase());
        return reinterpret_cast<uint8_t*>(LoneStubCalls[0]);
    }
    if (LoneStubCalls.empty() && ActivateSlotCalls.size() == 1)
    {
        const auto [Site, Target] = ActivateSlotCalls[0];
        Natives::AFortPlayerController_ActivateSlot = reinterpret_cast<decltype(Natives::AFortPlayerController_ActivateSlot)>(Target);
        Natives::AFortGameModeAthena_StartAircraftPhase = reinterpret_cast<decltype(Natives::AFortGameModeAthena_StartAircraftPhase)>(Finder::FunctionStart(Site));
        Log::Info(L"Finder: AFortGameModeAthena::StartAircraftPhase at 0x{:X} clears each player at 0x{:X}, its call to AFortPlayerController::ActivateSlot (0x{:X}), before the warmup pawn is destroyed",
            Finder::FunctionStart(Site) - Finder::ImageBase(), Site - Finder::ImageBase(), Target - Finder::ImageBase());
        return reinterpret_cast<uint8_t*>(Site);
    }
    Log::Info(L"Finder: no stripped player cleanup in AFortGameModeAthena::StartAircraftPhase in this build");
    return nullptr;
}

namespace
{
    std::vector<uint8_t*> StrippedStatEventCalls;
    std::vector<uint8_t*> StrippedTeamStatEventCalls;

    // A call with SendStatEventWithTags' typed wrapper's eight arguments: since the last call before it, four stack
    // arguments were stored (mov [rsp+20h/28h/30h/38h], of any width), and no fifth. SendEndPrimaryMission's place (7
    // arguments, 1.10 0x6FDF1B) and the other calls to the shared empty function don't have this.
    // A store to [rsp+40h] is no fifth argument when the same stretch also takes that slot's address (lea reg,
    // [rsp+40h]): it is a local that one of the arguments points at. From 6.00 the flush of a player's pawn's damage
    // (AFortPlayerController's, 6.02 0x1203940: an unflushed Damage event for each DamageDone entry of the pawn, sent by
    // AFortGameModeZone::Killed at 0xE57EA3 and by PawnPendingDestroy at 0x11FE48E) builds the event's empty target tags
    // there and passes them in r9 (0x1203A4F, 0x1203A7B). Taken for a fifth argument, its call (6.02 0x1203A96; 6.00
    // 0x12202F6, 6.01 0x11FF9B6, 6.10 0x1249C16, 6.20 0x12864D6) was left to the shared empty function, and no "Deal
    // damage" challenge moved (#420). Of all the calls to the shared empty function in those five images, this lets in
    // that one alone.
    bool IsStatEventCall(uintptr_t Site, bool* bOutFifthSlotIsLocal = nullptr)
    {
        uintptr_t Begin = Site > 0x60 ? Site - 0x60 : Site;
        for (uintptr_t Call = FindRawCall(Begin, Site); Call; Call = FindRawCall(Call + 1, Site))
        {
            Begin = Call + 5;
        }
        constexpr uint8_t FifthSlot = 0x40;
        uint32_t Stored = 0;
        bool bFifthSlotAddressTaken = false;
        for (uintptr_t Address = Begin; Address + 4 <= Site; ++Address)
        {
            const uint8_t Opcode = Byte(Address);
            if ((Opcode == 0x88 || Opcode == 0x89 || Opcode == 0xC6 || Opcode == 0xC7) && (Byte(Address + 1) & 0xC7) == 0x44 && Byte(Address + 2) == 0x24)
            {
                const uint8_t Displacement = Byte(Address + 3);
                if (Displacement >= 0x20 && Displacement <= FifthSlot && Displacement % 8 == 0)
                {
                    Stored |= 1u << ((Displacement - 0x20) / 8);
                }
            }
            else if (Opcode == 0x8D && (Byte(Address + 1) & 0xC7) == 0x44 && Byte(Address + 2) == 0x24 && Byte(Address + 3) == FifthSlot)
            {
                bFifthSlotAddressTaken = true;
            }
        }
        constexpr uint32_t FifthSlotBit = 1u << ((FifthSlot - 0x20) / 8);
        const bool bFifthSlotIsLocal = (Stored & FifthSlotBit) != 0 && bFifthSlotAddressTaken;
        if (bFifthSlotIsLocal)
        {
            Stored &= ~FifthSlotBit;
        }
        if (bOutFifthSlotIsLocal)
        {
            *bOutFifthSlotIsLocal = bFifthSlotIsLocal;
        }
        return Stored == 0xF;
    }

    // Whether the eight arguments of a stat event call start with the event type in cl rather than a quest manager in
    // rcx: the last instruction since the call before it that writes rcx writes only its low byte (mov cl, imm8;
    // mov cl, r/m8; movzx ecx, r/m8). 6.00 (CL-4395664) adds EFortQuestObjectiveStatEvent::TeamInteract and a static
    // sender for the instigator's team, which AFortPlayerController::ServerAttemptInteract_Implementation (0x1223EF0)
    // calls after the instigator's own Interact (0x1224327): `mov rdx, rsi; mov cl, 13h; call` at 0x1224354, the
    // controller second. It is stripped to the shared empty function as the typed wrapper is, so it looked like one
    // more sender, and Core's SendStatEventCall read the type byte (and whatever rcx held above it) as the quest manager
    // (#402). A typed wrapper call's rcx is the quest manager pointer (mov rcx, r64 / lea / load), never a byte.
    bool IsTeamStatEventCall(uintptr_t Site)
    {
        uintptr_t Begin = 0;
        for (uintptr_t Call = FindRawCall(Site > 0x60 ? Site - 0x60 : Site, Site); Call; Call = FindRawCall(Call + 1, Site))
        {
            Begin = Call + 5;
        }
        bool bTypeInCl = false;
        uintptr_t Address = Begin;
        while (Address && Address < Site)
        {
            hde64s Instruction{};
            const uint32_t Length = hde64_disasm(reinterpret_cast<const void*>(Address), &Instruction);
            if (!Length || (Instruction.flags & F_ERROR))
            {
                return false;
            }
            const uint8_t Opcode = Instruction.opcode;
            const bool bModRm = (Instruction.flags & F_MODRM) != 0;
            const bool bRegIsRcx = bModRm && Instruction.modrm_reg == 1 && !Instruction.rex_r;
            const bool bRmIsRcx = bModRm && Instruction.modrm_mod == 3 && Instruction.modrm_rm == 1 && !Instruction.rex_b;
            if ((Opcode == 0xB1 && !Instruction.rex_b) || (Opcode == 0x8A && bRegIsRcx) || (Opcode == 0x0F && Instruction.opcode2 == 0xB6 && bRegIsRcx))
            {
                bTypeInCl = true;
            }
            else if (((Opcode == 0x8B || Opcode == 0x8D || (Opcode == 0x0F && Instruction.opcode2 == 0xB7)) && bRegIsRcx) || ((Opcode == 0x89 || Opcode == 0x31 || Opcode == 0x33) && bRmIsRcx)
                || ((Opcode == 0xB9 || Opcode == 0xC7) && !Instruction.rex_b && (Opcode == 0xB9 || bRmIsRcx)))
            {
                bTypeInCl = false;
            }
            Address += Length;
        }
        return Address == Site && bTypeInCl;
    }

    // The displacement of the last call through a register's vtable (`call [reg+disp32]`) in [Begin, End), or -1.
    int32_t LastVirtualCallDisplacement(uintptr_t Begin, uintptr_t End)
    {
        int32_t Displacement = -1;
        for (uintptr_t Address = Begin; Address + 6 <= End; ++Address)
        {
            const uint8_t ModRm = Byte(Address + 1);
            if (Byte(Address) == 0xFF && (ModRm & 0xF8) == 0x90 && (ModRm & 0x07) != 0x04)
            {
                Displacement = *reinterpret_cast<const int32_t*>(Address + 2);
            }
        }
        return Displacement;
    }

    // 1.10 strips UFortQuestManager::SendStatEventWithTags and SaveStatEvents (their log strings are gone). Its typed
    // wrapper became the shared empty function, which the stat events' senders still call with the wrapper's arguments
    // (WaveComplete 0x6FA7C6, Interact 0x89B962, the game mode's kills 0x5CD7EF/0x5CD99D and team events 0x5CDF3A, the
    // SendQuestEvent cheat 0x850D59), and SendCustomStatEvent (0x90BEF0) only validates the stat. The senders are found
    // from the two that ask GetSourceAndContextTags for the event's tags just before (the shape the game's own senders
    // have, 1.9.1's 0x6BBA24/0x85C102): the quest manager getter they call through (`mov dl, 2; call [rax+1678h]`,
    // AFortPlayerController::GetQuestManager) is the vtable displacement every sender's function calls through, and every
    // eight-argument call to the shared empty function in such a function is a sender.
    bool ResolveStrippedQuestStatEvents()
    {
        // GetSourceAndContextTags is looked up by reflection, which needs the objects (a build that boots Core before
        // the engine has none yet, and such builds keep their stat events).
        if (!Natives::GUObjectArray || Natives::GUObjectArray->GetObjectArrayNum() == 0)
        {
            Log::Info(L"Finder: no objects yet, stripped stat events not looked for");
            return false;
        }
        const uintptr_t SendCustomStatEvent = FunctionOf(L"SendCustomStatEvent: Called with invalid or non custom stat %s!");
        if (!Natives::FFrame_Step)
        {
            Finder::ResolveScriptFrame();
        }
        const uintptr_t SourceAndContextTags = ExecImplementation(L"/Script/FortniteGame.FortQuestManager:GetSourceAndContextTags");
        const auto SharedEmpty = reinterpret_cast<uintptr_t>(Natives::SharedEmptyFunction);
        if (!SendCustomStatEvent || !SourceAndContextTags || !SharedEmpty)
        {
            Log::Error(L"Finder: stripped stat events not found (SendCustomStatEvent {}, GetSourceAndContextTags {}, shared empty function {})", SendCustomStatEvent != 0, SourceAndContextTags != 0, SharedEmpty != 0);
            return false;
        }

        int32_t GetterDisplacement = -1;
        bool bAgree = true;
        for (const uintptr_t Site : Finder::CallsTo(SourceAndContextTags, false))
        {
            const uintptr_t Next = FindRawCall(Site + 5, Site + 0x60);
            const uintptr_t Function = Next && Finder::RelativeTarget(Next, 1, 5) == SharedEmpty && IsStatEventCall(Next) ? Finder::FunctionStart(Site) : 0;
            const int32_t Displacement = Function ? LastVirtualCallDisplacement(Function, Site) : -1;
            if (Displacement > 0)
            {
                bAgree &= GetterDisplacement < 0 || GetterDisplacement == Displacement;
                GetterDisplacement = Displacement;
            }
        }
        if (GetterDisplacement <= 0 || !bAgree)
        {
            Log::Error(L"Finder: stripped stat events' quest manager getter not found (displacement 0x{:X}, {})", GetterDisplacement, bAgree ? L"agreed" : L"disagreed");
            return false;
        }

        // 2.5's AFortMission::RemoveParticipantAccount (0xC87370) also gets the quest manager through the getter and calls
        // the shared empty function with eight arguments (0xC875E0), where UFortQuestManager::SendEndPrimaryMission was:
        // that one is no stat event (Core sends EndPrimaryMission itself, ResolveZoneEndLive).
        const std::vector<std::pair<uintptr_t, uintptr_t>> EndMissionChunks = Finder::FunctionChunks(ExecImplementation(L"/Script/FortniteGame.FortMission:RemoveParticipantAccount"));
        const auto IsInEndMission = [&EndMissionChunks](uintptr_t Site)
        {
            return std::any_of(EndMissionChunks.begin(), EndMissionChunks.end(), [Site](const auto& Chunk) { return Site >= Chunk.first && Site < Chunk.second; });
        };
        std::vector<uint8_t*> Calls;
        std::vector<uint8_t*> TeamCalls;
        std::vector<uint8_t*> LocalFifthSlotCalls;
        for (const uintptr_t Site : Finder::CallsTo(SharedEmpty, false))
        {
            bool bFifthSlotIsLocal = false;
            const uintptr_t Function = Byte(Site) == 0xE8 && IsStatEventCall(Site, &bFifthSlotIsLocal) && !IsInEndMission(Site) ? Finder::FunctionStart(Site) : 0;
            if (Function && Function < Site)
            {
                bool bCallsGetter = false;
                for (uintptr_t Address = Function; Address + 6 <= Site && !bCallsGetter; ++Address)
                {
                    const uint8_t ModRm = Byte(Address + 1);
                    bCallsGetter = Byte(Address) == 0xFF && (ModRm & 0xF8) == 0x90 && (ModRm & 0x07) != 0x04 && *reinterpret_cast<const int32_t*>(Address + 2) == GetterDisplacement;
                }
                if (bCallsGetter)
                {
                    (IsTeamStatEventCall(Site) ? TeamCalls : Calls).push_back(reinterpret_cast<uint8_t*>(Site));
                    if (bFifthSlotIsLocal)
                    {
                        LocalFifthSlotCalls.push_back(reinterpret_cast<uint8_t*>(Site));
                    }
                }
            }
        }
        if (Calls.empty())
        {
            Log::Error(L"Finder: stripped stat events' sender calls not found (quest manager getter [vtable+0x{:X}])", GetterDisplacement);
            return false;
        }

        StrippedStatEventCalls = std::move(Calls);
        StrippedTeamStatEventCalls = std::move(TeamCalls);
        Natives::UFortQuestManager_SendCustomStatEvent = reinterpret_cast<decltype(Natives::UFortQuestManager_SendCustomStatEvent)>(SendCustomStatEvent);
        Natives::UFortQuestManager_bStatEventsStripped = true;
        const auto Describe = [](const std::vector<uint8_t*>& List)
        {
            std::wstring Sites;
            for (const uint8_t* Call : List)
            {
                Sites += std::format(L"{}0x{:X}", Sites.empty() ? L"" : L", ", reinterpret_cast<uintptr_t>(Call) - Finder::ImageBase());
            }
            return Sites;
        };
        Log::Info(L"Finder: UFortQuestManager::SendStatEventWithTags and SaveStatEvents are stripped, Core sends the stat events: SendCustomStatEvent at 0x{:X}, quest manager getter [vtable+0x{:X}], {} sender call(s): {}",
            SendCustomStatEvent - Finder::ImageBase(), GetterDisplacement, StrippedStatEventCalls.size(), Describe(StrippedStatEventCalls));
        if (!StrippedTeamStatEventCalls.empty())
        {
            Log::Info(L"Finder: {} team stat event sender call(s) (the type in cl, the instigating controller second): {}", StrippedTeamStatEventCalls.size(), Describe(StrippedTeamStatEventCalls));
        }
        if (!LocalFifthSlotCalls.empty())
        {
            Log::Info(L"Finder: {} of them store [rsp+40h] as a local their arguments point at, not as a fifth argument: {}", LocalFifthSlotCalls.size(), Describe(LocalFifthSlotCalls));
        }
        return true;
    }
}

const std::vector<uint8_t*>& Finder::StrippedQuestStatEventCalls()
{
    return StrippedStatEventCalls;
}

const std::vector<uint8_t*>& Finder::StrippedQuestTeamStatEventCalls()
{
    return StrippedTeamStatEventCalls;
}

bool Finder::ResolveQuestStatEvents()
{
    static bool bSearched = false;
    if (Natives::UFortQuestManager_FlushStatEvents || Natives::UFortQuestManager_bStatEventsStripped || bSearched)
    {
        return Natives::UFortQuestManager_FlushStatEvents || Natives::UFortQuestManager_bStatEventsStripped;
    }
    bSearched = true;

    // UFortQuestManager::SendStatEventWithTags (1.8 0x890730), which every stat event goes through (SendStatEvent,
    // 0x8918D0, is its wrapper), logs the client string. When an event asked for a flush it hands the quest manager's
    // progress to its flush (0x88E060, at 0x891792): with nothing pending that only runs the delegate it is given, else
    // it calls SaveStatEvents (0x891940, the "SaveStatEvents for" log), which sends UpdateQuests to the quest manager's
    // profile. The flush is the call in SendStatEventWithTags that calls SaveStatEvents; 1.7.2 has the same three
    // (0x86F450, 0x86CD60, 0x870690).
    const uintptr_t SendStatEventWithTags = FunctionOf(L"SendStatEventWithTags: Cannot be called on the client!");
    const uintptr_t SaveStatEvents = FunctionOf(L"SaveStatEvents for %s: %s");
    const uintptr_t SendEnd = SendStatEventWithTags ? Finder::FunctionEnd(SendStatEventWithTags) : 0;
    uintptr_t Flush = 0;
    for (const auto& [Site, Target] : SendEnd && SaveStatEvents ? Finder::DirectCalls(SendStatEventWithTags, SendEnd) : std::vector<std::pair<uintptr_t, uintptr_t>>())
    {
        const uintptr_t TargetEnd = Flush || !IsInText(Target) ? 0 : Finder::FunctionEnd(Target);
        if (TargetEnd > Target && TargetEnd - Target < 0x400 && CallWithin(Target, TargetEnd - Target, SaveStatEvents))
        {
            Flush = Target;
        }
    }
    // 2.4.2 puts the client string back in a different function (0x477780, which hands the event to registered
    // listeners through slot 0x47), but the quest manager's SendStatEventWithTags and SaveStatEvents stay stripped.
    if (!Flush && !SaveStatEvents && ResolveStrippedQuestStatEvents())
    {
        return true;
    }
    if (!Flush)
    {
        Log::Error(L"Finder: UFortQuestManager's stat event flush not found (SendStatEventWithTags {}, SaveStatEvents {})", SendStatEventWithTags != 0, SaveStatEvents != 0);
        return false;
    }

    Natives::UFortQuestManager_SendStatEventWithTags = reinterpret_cast<decltype(Natives::UFortQuestManager_SendStatEventWithTags)>(SendStatEventWithTags);
    Natives::UFortQuestManager_FlushStatEvents = reinterpret_cast<decltype(Natives::UFortQuestManager_FlushStatEvents)>(Flush);
    Log::Info(L"Finder: UFortQuestManager::SendStatEventWithTags at 0x{:X} flushes at 0x{:X}", SendStatEventWithTags - Finder::ImageBase(), Flush - Finder::ImageBase());
    return true;
}

bool Finder::ResolveAthenaHeroLive()
{
    static bool bSearched = false;
    if (Natives::AFortPlayerController_GetOrInitializeHeroIndex >= 0 || bSearched)
    {
        return Natives::AFortPlayerController_GetOrInitializeHeroIndex >= 0;
    }
    bSearched = true;

    // AFortPlayerController::GetOrInitializeHero (1.7.2 0x80BA00, the account's hero) and AFortPlayerControllerAthena's
    // override (0x42E5E0, the next of the controller's DefaultHeroes, slot 0x2C8) both log this string. The walk can run
    // into the vtables after the Athena controller's, where the base function sits further on: the lowest slot is it.
    void** VTable = CdoVTable(L"/Script/FortniteGame.FortPlayerControllerAthena");
    int32_t Index = -1;
    for (const uintptr_t Ref : Finder::StringRefs(L"GetOrInitializeHero called with valid player info"))
    {
        const int32_t Found = FirstVTableIndex(VTable, Finder::FunctionStart(Ref));
        Index = Found >= 0 && (Index < 0 || Found < Index) ? Found : Index;
    }
    if (Index < 0)
    {
        Log::Error(L"Finder: AFortPlayerController::GetOrInitializeHero slot not found");
        return false;
    }

    Natives::AFortPlayerController_GetOrInitializeHeroIndex = Index;
    Log::Info(L"Finder: AFortPlayerController::GetOrInitializeHero is slot 0x{:X}", Index);
    return true;
}

bool Finder::ResolveAthenaMenuHero()
{
    // UFortUITeamInfo::GetTeamMemberInfo (1.7.2 0x795300) fills a lobby pad's FFortTeamMemberInfo from a registered player.
    // In Battle Royale its HeroItem is the player info's AthenaMenuHeroDef, which while there is none it makes from one of
    // UFortGameData::DefaultAthenaHeroes picked with rand() (the import call at 0x7955DB, its only one). 1.1 and 1.2 have
    // no Battle Royale and call no rand there.
    // FUniqueNetId's IsValid and ToString slots, which the voice HUD also reads: resolved even when the lobby picks no
    // random hero (1.8 shows the player's Athena loadout instead, 0x79EB60). Builds without Battle Royale's HUD (1.1, 1.2)
    // keep leaving them to the pick, as before.
    const auto ResolveUniqueNetIdSlots = []
    {
        // AFortPlayerController::InitializeMcpProfile's failure log prints the player's id: IsValid (`call [rax+18h];
        // test al, al`), then ToString, the virtual call nearest the string.
        const uintptr_t ProfileFailed = OnlyRef(L"Failed to initialize profile for player: UniqueId:[%s] GameAccountId=[%s]");
        const uintptr_t ToString = ProfileFailed ? FindBackward(ProfileFailed, 0x60, { 0xFF, 0x50, -1 }) : 0;
        const uintptr_t IsValid = ToString ? FindBackward(ToString, 0x20, { 0xFF, 0x50, -1, 0x84, 0xC0 }) : 0;
        const int32_t ToStringIndex = ToString && Byte(ToString + 2) % 8 == 0 ? Byte(ToString + 2) / 8 : -1;
        const int32_t IsValidIndex = IsValid && Byte(IsValid + 2) % 8 == 0 ? Byte(IsValid + 2) / 8 : -1;
        if (ToStringIndex < 0 || IsValidIndex < 0 || IsValidIndex >= ToStringIndex)
        {
            Log::Error(L"Finder: FUniqueNetId::IsValid and ToString slots not found");
            return false;
        }
        Natives::FUniqueNetId_IsValidIndex = IsValidIndex;
        Natives::FUniqueNetId_ToStringIndex = ToStringIndex;
        return true;
    };

    const uintptr_t GetTeamMemberInfo = FunctionOf(L"GetTeamMemberInfo: [%s] Team member info was not filled");
    const uintptr_t GetTeamMemberInfoEnd = GetTeamMemberInfo ? Finder::FunctionEnd(GetTeamMemberInfo) : 0;
    const uintptr_t RandSlot = Finder::ImportSlot("api-ms-win-crt-utility-l1-1-0.dll", "rand");
    std::vector<uintptr_t> RandCalls;
    for (uintptr_t Call = GetTeamMemberInfoEnd && RandSlot ? FindInstruction(GetTeamMemberInfo, GetTeamMemberInfoEnd, { 0xFF, 0x15 }) : 0; Call; Call = FindInstruction(Call + 1, GetTeamMemberInfoEnd, { 0xFF, 0x15 }))
    {
        if (Finder::RelativeTarget(Call, 2, 6) == RandSlot)
        {
            RandCalls.push_back(Call);
        }
    }
    if (!GetTeamMemberInfo || RandCalls.empty())
    {
        Log::Info(L"Finder: UFortUITeamInfo::GetTeamMemberInfo picks no Battle Royale menu hero in this build");
        if (!Finder::StringRefs(L"&ThisClass::HandleUIGameplayCue").empty())
        {
            ResolveUniqueNetIdSlots();
        }
        return false;
    }
    if (RandCalls.size() != 1)
    {
        Log::Error(L"Finder: expected 1 rand call in UFortUITeamInfo::GetTeamMemberInfo, found {}", RandCalls.size());
        return false;
    }

    if (!ResolveUniqueNetIdSlots())
    {
        return false;
    }

    Natives::UFortUITeamInfo_GetTeamMemberInfo = reinterpret_cast<decltype(Natives::UFortUITeamInfo_GetTeamMemberInfo)>(GetTeamMemberInfo);
    Sites::TeamMemberInfoAthenaMenuHeroRand = reinterpret_cast<uint8_t*>(RandCalls.front());
    Log::Info(L"Finder: UFortUITeamInfo::GetTeamMemberInfo at 0x{:X} picks the Battle Royale menu hero at 0x{:X}", GetTeamMemberInfo, RandCalls.front());
    return true;
}

bool Finder::ResolveAthenaHUDPawnBinding()
{
    // UAthenaHUDContext binds HandleUIGameplayCue and HandleLocalPlayerDBNOStateChanged to its player controller's pawn
    // (AFortPlayerPawnAthena::OnUIGameplayCue, OnDBNOStateChanged) from an event of its local player (1.7.2 0x2D64E70),
    // the only function that adds either by name. 1.1 and 1.2 have no Battle Royale HUD.
    const std::vector<uintptr_t> CueRefs = Finder::StringRefs(L"&ThisClass::HandleUIGameplayCue");
    const std::vector<uintptr_t> DBNORefs = Finder::StringRefs(L"&ThisClass::HandleLocalPlayerDBNOStateChanged");
    const uintptr_t BindLocalPawn = CueRefs.size() == 1 ? Finder::FunctionStart(CueRefs.front()) : 0;
    if (CueRefs.empty())
    {
        Log::Info(L"Finder: no Battle Royale HUD pawn binding in this build");
        return false;
    }
    if (!BindLocalPawn || DBNORefs.size() != 1 || Finder::FunctionStart(DBNORefs.front()) != BindLocalPawn)
    {
        Log::Error(L"Finder: UAthenaHUDContext's pawn binding not found ({} and {} references)", CueRefs.size(), DBNORefs.size());
        return false;
    }

    Natives::UAthenaHUDContext_BindLocalPawn = reinterpret_cast<decltype(Natives::UAthenaHUDContext_BindLocalPawn)>(BindLocalPawn);
    Log::Info(L"Finder: UAthenaHUDContext binds the local pawn at 0x{:X}", BindLocalPawn - Finder::ImageBase());
    return true;
}

bool Finder::ResolveMcpProfileQuery()
{
    // UMcpProfile::QueryProfile(const FMcpQueryComplete&) (1.8.1 0x3C48DD0, 1.8 0x3C34980, 1.7.2 0x3B4A5E0) picks the
    // request's URL context, McpDedicatedServerCommandUrl, McpClientCommandUrl or (another account's profile)
    // McpPublicCommandUrl, and calls QueryProfile(FBaseUrlContext&) (1.8.1 0x3C48A80), the only function that logs an
    // overlapping query. Two other functions name McpPublicCommandUrl; only this one calls it (1.8.2: 0x3C52470, calling
    // 0x3C52120). 1.11 made QueryProfile(FBaseUrlContext&) virtual (UFortMcpProfile's vtable +0x238 on 1.11 to 3.0), and
    // QueryProfile calls it through the vtable (2.5 0x79D250 calling 0x79D3F0, 3.0 0x7A1800 calling 0x7A19A0): there the
    // function's virtual call is at the displacement of a vtable slot that holds it. The other two functions only call
    // through +0x28 or +0x48.
    const uintptr_t QueryWithContext = FunctionOf(L"Caught overlapping attempts to query same user's profile, will be called when previous call completes. User %s");
    std::vector<int32_t> QueryDisplacements;
    for (void** Slot : Finder::ReadOnlySlotsHolding(QueryWithContext))
    {
        // The slot's displacement in its vtable: the code pointers that come before it, back to the vtable's start.
        int32_t Index = 0;
        while (Index < 0x400 && Finder::IsReadable(Slot - Index - 1, sizeof(void*)) &&
               reinterpret_cast<uintptr_t>(Slot[-Index - 1]) >= Finder::TextBegin() && reinterpret_cast<uintptr_t>(Slot[-Index - 1]) < Finder::TextEnd())
        {
            ++Index;
        }
        QueryDisplacements.push_back(Index * static_cast<int32_t>(sizeof(void*)));
    }
    std::vector<uintptr_t> Candidates;
    for (const uintptr_t Ref : QueryWithContext ? Finder::StringRefs(L"McpPublicCommandUrl") : std::vector<uintptr_t>{})
    {
        const uintptr_t Function = Finder::FunctionStart(Ref);
        const uintptr_t End = Function ? Finder::FunctionEnd(Function) : 0;
        const std::vector<std::pair<uintptr_t, uintptr_t>> Calls = End ? Finder::DirectCalls(Function, End) : std::vector<std::pair<uintptr_t, uintptr_t>>{};
        const int32_t VirtualDisplacement = End ? LastVirtualCallDisplacement(Function, End) : -1;
        const bool bCallsQuery = std::any_of(Calls.begin(), Calls.end(), [QueryWithContext](const auto& Call) { return Call.second == QueryWithContext; }) ||
                                 (VirtualDisplacement >= 0 && std::find(QueryDisplacements.begin(), QueryDisplacements.end(), VirtualDisplacement) != QueryDisplacements.end());
        if (bCallsQuery && std::find(Candidates.begin(), Candidates.end(), Function) == Candidates.end())
        {
            Candidates.push_back(Function);
        }
    }
    if (Candidates.size() != 1)
    {
        Log::Error(L"Finder: UMcpProfile::QueryProfile not found ({} candidates)", Candidates.size());
        return false;
    }

    Natives::UMcpProfile_QueryProfile = reinterpret_cast<decltype(Natives::UMcpProfile_QueryProfile)>(Candidates.front());
    Log::Info(L"Finder: UMcpProfile::QueryProfile at 0x{:X}", Candidates.front() - Finder::ImageBase());
    return true;
}

bool Finder::ResolveDBNOReviveLive(ptrdiff_t IsDBNOOffset, ptrdiff_t QuickBarsOffset)
{
    static bool bSearched = false;
    static bool bResolved = false;
    if (bSearched)
    {
        return bResolved;
    }
    bSearched = true;

    // The game code 1.8's AFortPlayerPawn::ServerReviveFromDBNO_Implementation (0x7FF160) runs, which 1.8.1 strips with
    // the implementation (its slot holds the shared empty function). Each piece is still called from code 1.8.1 keeps.
    // 1.8.2 (CL-3741772) strips it too, and gives the same slots apart from the controller's: OnPawnRevived is 0x300
    // there (1.8.1 0x301), and the DBNO timer is +0x1678.
    void** PawnVTable = CdoVTable(L"/Script/FortniteGame.FortPlayerPawn");
    void** ZoneVTable = CdoVTable(L"/Script/FortniteGame.FortPlayerControllerZone");
    void** AthenaControllerVTable = CdoVTable(L"/Script/FortniteGame.FortPlayerControllerAthena");
    void** AbilitySystemVTable = CdoVTable(L"/Script/GameplayAbilities.AbilitySystemComponent");

    // The native an exec thunk ends in, by a call or a tail jump, through a jump thunk (1.8.1's
    // AFortPlayerPawn::execReviveFromDBNO calls 0x809950, "jmp 0xA514A0"). A thunk that only forwards is a leaf without
    // unwind data, so it has no known end: 1.8.1's AFortAIPawn::execReviveFromDBNO (0xB36A40) steps the frame's code and
    // ends 0x15 bytes in with "jmp 0x7D0B70", the padding right after it. 2.1's (0xA0A1D0) has a nop after its
    // "jmp 0x81AE30" instead of padding, so a tail jump to the start of a function with unwind data counts too.
    const auto ExecNative = [](const wchar_t* FunctionPath)
    {
        UFunction* Function = FindObject<UFunction>(nullptr, FunctionPath);
        const uintptr_t Exec = Function ? reinterpret_cast<uintptr_t>(Function->GetNativeFunc()) : 0;
        const uintptr_t ExecEnd = IsInText(Exec) ? Finder::FunctionEnd(Exec) : 0;
        uintptr_t Native = 0;
        for (uintptr_t Site = Exec; ExecEnd && Site + 5 <= ExecEnd; ++Site)
        {
            if ((Byte(Site) == 0xE8 || Byte(Site) == 0xE9) && IsInText(Finder::RelativeTarget(Site, 1, 5)))
            {
                Native = Finder::RelativeTarget(Site, 1, 5);
            }
        }
        for (uintptr_t Site = Exec; !ExecEnd && IsInText(Exec) && Site < Exec + 0x40 && !Native; ++Site)
        {
            if (Byte(Site) == 0xE9 && IsInText(Finder::RelativeTarget(Site, 1, 5)) && (Byte(Site + 5) == 0xCC || Finder::FunctionEnd(Finder::RelativeTarget(Site, 1, 5))))
            {
                Native = Finder::RelativeTarget(Site, 1, 5);
            }
        }
        return Native && Byte(Native) == 0xE9 ? Finder::RelativeTarget(Native, 1, 5) : Native;
    };

    // A function's end, past the chained unwind fragments that continue it: FunctionEnd only joins fragments that start
    // where the last ends, and 1.8.1's AFortAIPawn::ReviveFromDBNO (0x7D0B70-0x7D0BA5) goes on in one 4 bytes later
    // (0x7D0BA9-0x7D0E60), which holds all its calls.
    const auto FunctionAndFragmentsEnd = [](uintptr_t Function)
    {
        uintptr_t End = Function ? Finder::FunctionEnd(Function) : 0;
        while (End)
        {
            uintptr_t Fragment = 0;
            for (uintptr_t Probe = End; Probe < End + 0x10 && !Fragment; ++Probe)
            {
                if (Finder::FunctionStart(Probe) == Function && Finder::FunctionEnd(Probe))
                {
                    Fragment = Probe;
                }
            }
            if (!Fragment)
            {
                break;
            }
            End = Finder::FunctionEnd(Fragment);
        }
        return End;
    };

    // CanReviveFromDBNO (slot 0x199): the interaction that revives a teammate (1.8 0x88DFF0, 1.8.1 0x895ED0) asks it just
    // before it calls ServerReviveFromDBNO, through the call stub execReviveFromDBNO ends in (1.8 0xA46330).
    const uintptr_t ServerReviveCall = ExecNative(L"/Script/FortniteGame.FortPlayerPawn:ReviveFromDBNO");
    int32_t CanReviveIndex = -1;
    int32_t CanReviveSlots = 0;
    for (const uintptr_t Site : ServerReviveCall ? Finder::CallsTo(ServerReviveCall, false) : std::vector<uintptr_t>())
    {
        const int32_t Slot = VirtualSlot(FindVirtualCall(Site - 0x20, Site));
        if (Slot >= 0 && Slot != CanReviveIndex)
        {
            CanReviveIndex = Slot;
            ++CanReviveSlots;
        }
    }
    if (CanReviveSlots != 1 || !VTableEntry(PawnVTable, CanReviveIndex))
    {
        CanReviveIndex = -1;
    }

    // StopNativeAction (slot 0x154): the pawn's slot jumps to UFortAbilitySystemComponent's (by its log string, 1.8
    // 0x2ACEF0) with the pawn's AbilitySystemComponent.
    const uintptr_t AbilitySystemStop = FunctionOf(L"Can't stop native action %s, not active!");
    const int32_t StopIndex = AbilitySystemStop ? FirstVTableIndexWhere(PawnVTable, [AbilitySystemStop](uintptr_t Entry)
    {
        for (uintptr_t Site = Entry; Site < Entry + 0x10; ++Site)
        {
            if ((Byte(Site) == 0x0F && Byte(Site + 1) == 0x85 && Finder::RelativeTarget(Site, 2, 6) == AbilitySystemStop) || (Byte(Site) == 0xE9 && Finder::RelativeTarget(Site, 1, 5) == AbilitySystemStop))
            {
                return true;
            }
        }
        return false;
    }) : -1;

    // CancelAbilities: StopNativeAction's first virtual call is ApplyAbilityBlockAndCancelTags (slot 0xAD), and
    // UAbilitySystemComponent's (1.8 0x3E21000) ends with CancelAbilities(&CancelTags, nullptr, RequestingAbility).
    const uintptr_t StopEnd = AbilitySystemStop ? Finder::FunctionEnd(AbilitySystemStop) : 0;
    const uintptr_t BlockAndCancel = StopEnd ? VTableEntry(AbilitySystemVTable, VirtualSlot(FindVirtualCall(AbilitySystemStop, StopEnd))) : 0;
    const uintptr_t BlockAndCancelEnd = BlockAndCancel ? Finder::FunctionEnd(BlockAndCancel) : 0;
    const std::vector<std::pair<uintptr_t, uintptr_t>> BlockAndCancelCalls = BlockAndCancelEnd ? Finder::DirectCalls(BlockAndCancel, BlockAndCancelEnd) : std::vector<std::pair<uintptr_t, uintptr_t>>();
    const uintptr_t CancelAbilities = BlockAndCancelCalls.empty() ? 0 : BlockAndCancelCalls.back().second;

    // UpdateDBNOInteractCollision (1.8 0x80CC80): the only reader of "DBNOInteractCollision", the box teammates revive
    // through, made while DBNO and destroyed otherwise.
    const std::vector<uintptr_t> CollisionRefs = Finder::StringRefs(L"DBNOInteractCollision");
    const uintptr_t UpdateCollision = CollisionRefs.size() == 1 ? Finder::FunctionStart(CollisionRefs.front()) : 0;

    // The DBNO timer, which kills the pawn when it runs out: StartDBNO (slot 0x18B, 1.8 0x805D90) sets bIsDBNO, calls
    // UpdateDBNOInteractCollision, then sets it with "lea rdx, [pawn + handle]" (1.8 +0x1660, 1.8.1 +0x1670).
    ptrdiff_t TimerOffset = -1;
    if (UpdateCollision && IsDBNOOffset > 0)
    {
        FirstVTableIndexWhere(PawnVTable, [&](uintptr_t Entry)
        {
            const uintptr_t End = Finder::FunctionEnd(Entry);
            for (uintptr_t Site = Entry; End && Site + 7 <= End; ++Site)
            {
                const uint8_t SetModRm = Byte(Site + 1);
                if (Byte(Site) != 0xC6 || (SetModRm & 0xF8) != 0x80 || (SetModRm & 0x07) == 0x04 || *reinterpret_cast<const int32_t*>(Site + 2) != IsDBNOOffset || Byte(Site + 6) != 1)
                {
                    continue;
                }
                const uintptr_t CallsEnd = std::min(End, Site + 0x40);
                for (uintptr_t Call = FindRawCall(Site, CallsEnd); Call; Call = FindRawCall(Call + 1, CallsEnd))
                {
                    if (Finder::RelativeTarget(Call, 1, 5) != UpdateCollision)
                    {
                        continue;
                    }
                    const uintptr_t Lea = FindInstruction(Call, std::min(End, Call + 0x200), { 0x48, 0x8D, 0x90 | (SetModRm & 0x07) });
                    if (Lea)
                    {
                        TimerOffset = *reinterpret_cast<const int32_t*>(Lea + 3);
                        return true;
                    }
                }
            }
            return false;
        });
    }

    // OnPawnRevived (AFortPlayerControllerZone slot 0x2FF, 1.8 0x886050): counts the revive as SelfRevives or
    // RevivesFromTeammate in the controller's StatManager, finding the stat (0x8A16E0) and adding 1 to it (0x8A9840)
    // right after the name. The other reader of these names registers the stats.
    int32_t OnPawnRevivedIndex = -1;
    uintptr_t FindStat = 0;
    uintptr_t IncrementStat = 0;
    for (const uintptr_t Ref : Finder::StringRefs(L"RevivesFromTeammate"))
    {
        const uintptr_t Function = Finder::FunctionStart(Ref);
        const int32_t Index = FirstVTableIndex(ZoneVTable, Function);
        const uintptr_t End = Index >= 0 ? Finder::FunctionEnd(Function) : 0;
        if (!End)
        {
            continue;
        }
        OnPawnRevivedIndex = Index;
        const std::vector<std::pair<uintptr_t, uintptr_t>> Calls = Finder::DirectCalls(Ref, End);
        if (Calls.size() == 3)
        {
            FindStat = Calls[1].second;
            IncrementStat = Calls[2].second;
            // 1.10 loads the amount, 1.0, as a double between the two (movsd xmm1, [rip+...]: F2 0F 10 0D).
            for (uintptr_t Site = Calls[1].first + 5; Site + 4 <= Calls[2].first; ++Site)
            {
                if (Byte(Site) == 0xF2 && Byte(Site + 1) == 0x0F && Byte(Site + 2) == 0x10 && Byte(Site + 3) == 0x0D)
                {
                    Natives::FStat_bDoubleAmount = true;
                    break;
                }
            }
        }
    }

    // The resurrect durability penalty, the slot after OnPawnRevived (1.8 0x865BE0): it takes the death penalty's amount
    // off the quickbar weapons, reading QuickBars first.
    const auto ReadsQuickBars = [QuickBarsOffset](uintptr_t Function, uint8_t Opcode, uint8_t ModRmMask, uint8_t ModRm)
    {
        for (uintptr_t Site = Function; Function && QuickBarsOffset > 0 && Site < Function + 0x20; ++Site)
        {
            if (Byte(Site) == 0x48 && Byte(Site + 1) == Opcode && (Byte(Site + 2) & ModRmMask) == ModRm && *reinterpret_cast<const int32_t*>(Site + 3) == QuickBarsOffset)
            {
                return true;
            }
        }
        return false;
    };
    const uintptr_t DurabilityPenalty = OnPawnRevivedIndex >= 0 ? VTableEntry(ZoneVTable, OnPawnRevivedIndex + 1) : 0;
    const int32_t DurabilityPenaltyIndex = DurabilityPenalty && !Finder::IsStubFunction(DurabilityPenalty) && ReadsQuickBars(DurabilityPenalty, 0x8B, 0xC7, 0x81) ? OnPawnRevivedIndex + 1 : -1;

    // RefocusQuickBars (1.8 0x844170): the call of AFortPlayerControllerZone::ClientOnPawnDied_Implementation (0x86A660)
    // that starts with "cmp qword ptr [rcx + QuickBars], 0", the quickbar focus a player gets back.
    UFunction* ClientOnPawnDied = FindObject<UFunction>(nullptr, L"/Script/FortniteGame.FortPlayerControllerZone:ClientOnPawnDied");
    const uintptr_t PawnDied = ClientOnPawnDied ? VTableEntry(ZoneVTable, Finder::ImplementationIndex(ClientOnPawnDied)) : 0;
    const uintptr_t PawnDiedEnd = PawnDied ? Finder::FunctionEnd(PawnDied) : 0;
    uintptr_t RefocusQuickBars = 0;
    for (const auto& [Site, Target] : PawnDiedEnd ? Finder::DirectCalls(PawnDied, PawnDiedEnd) : std::vector<std::pair<uintptr_t, uintptr_t>>())
    {
        if (ReadsQuickBars(Target, 0x83, 0xFF, 0xB9))
        {
            RefocusQuickBars = Target;
            break;
        }
    }

    // ForgetPawnForAllAI (1.8 0x2FDC20): AFortAIPawn's ReviveFromDBNO (0x7B5620) calls it right before it gets the
    // gameplay tags, adds the DBNO one to a container and cancels those abilities: the call before those two that it
    // makes once.
    const uintptr_t AIRevive = ExecNative(L"/Script/FortniteGame.FortAIPawn:ReviveFromDBNO");
    const uintptr_t AIReviveEnd = FunctionAndFragmentsEnd(AIRevive);
    const std::vector<std::pair<uintptr_t, uintptr_t>> AIReviveCalls = AIReviveEnd ? Finder::DirectCalls(AIRevive, AIReviveEnd) : std::vector<std::pair<uintptr_t, uintptr_t>>();
    uintptr_t ForgetPawn = 0;
    for (size_t Cancel = 2; CancelAbilities && Cancel < AIReviveCalls.size(); ++Cancel)
    {
        if (AIReviveCalls[Cancel].second != CancelAbilities)
        {
            continue;
        }
        for (size_t Index = Cancel - 2; Index-- > 0;)
        {
            const uintptr_t Target = AIReviveCalls[Index].second;
            if (std::count_if(AIReviveCalls.begin(), AIReviveCalls.end(), [Target](const std::pair<uintptr_t, uintptr_t>& Call) { return Call.second == Target; }) == 1)
            {
                ForgetPawn = Target;
                break;
            }
        }
        break;
    }

    // AFortPlayerControllerAthena::ServerDBNOReviveInterrupted_Implementation (1.8 0x44AC90), which 1.8.1 keeps: once
    // the pawn it's given has no reviver left, it ends that pawn's "being revived" tag and cue, as the Athena revive
    // does after getting up.
    UFunction* ReviveInterrupted = FindObject<UFunction>(nullptr, L"/Script/FortniteGame.FortPlayerControllerAthena:ServerDBNOReviveInterrupted");
    const int32_t InterruptedIndex = ReviveInterrupted ? Finder::ImplementationIndex(ReviveInterrupted) : -1;
    const uintptr_t Interrupted = VTableEntry(AthenaControllerVTable, InterruptedIndex);

    if (CanReviveIndex < 0 || StopIndex < 0 || !CancelAbilities || !UpdateCollision || TimerOffset <= 0 || OnPawnRevivedIndex < 0)
    {
        Log::Error(L"Finder: DBNO revive not found (CanReviveFromDBNO slot {} from {} sites, StopNativeAction slot {}, CancelAbilities 0x{:X}, UpdateDBNOInteractCollision 0x{:X}, DBNO timer +0x{:X}, OnPawnRevived slot {})",
            CanReviveIndex, CanReviveSlots, StopIndex, CancelAbilities ? CancelAbilities - Finder::ImageBase() : 0, UpdateCollision ? UpdateCollision - Finder::ImageBase() : 0, TimerOffset, OnPawnRevivedIndex);
        return false;
    }

    Natives::AFortPlayerPawn_CanReviveFromDBNOIndex = CanReviveIndex;
    Natives::AFortPawn_StopNativeActionIndex = StopIndex;
    Natives::UAbilitySystemComponent_CancelAbilities = reinterpret_cast<decltype(Natives::UAbilitySystemComponent_CancelAbilities)>(CancelAbilities);
    Natives::AFortPlayerPawn_UpdateDBNOInteractCollision = reinterpret_cast<decltype(Natives::AFortPlayerPawn_UpdateDBNOInteractCollision)>(UpdateCollision);
    Natives::AFortPlayerPawn_DBNOTimerHandle = TimerOffset;
    Natives::AFortPlayerControllerZone_OnPawnRevivedIndex = OnPawnRevivedIndex;
    Natives::AFortPlayerControllerZone_ApplyResurrectDurabilityPenaltyIndex = DurabilityPenaltyIndex;
    Natives::AFortPlayerController_RefocusQuickBars = reinterpret_cast<decltype(Natives::AFortPlayerController_RefocusQuickBars)>(RefocusQuickBars);
    Natives::AFortPawn_ForgetPawnForAllAI = reinterpret_cast<decltype(Natives::AFortPawn_ForgetPawnForAllAI)>(ForgetPawn);
    Natives::UStatManager_FindStat = reinterpret_cast<decltype(Natives::UStatManager_FindStat)>(FindStat);
    Natives::FStat_Increment = reinterpret_cast<decltype(Natives::FStat_Increment)>(IncrementStat);
    Natives::AFortPlayerControllerAthena_ServerDBNOReviveInterruptedIndex = Interrupted && !Finder::IsStubFunction(Interrupted) ? InterruptedIndex : -1;
    bResolved = true;

    const auto Rva = [](uintptr_t Address) { return Address ? Address - Finder::ImageBase() : 0; };
    Log::Info(L"Finder: DBNO revive: CanReviveFromDBNO slot 0x{:X}, StopNativeAction slot 0x{:X}, CancelAbilities 0x{:X}, UpdateDBNOInteractCollision 0x{:X}, DBNO timer +0x{:X}, OnPawnRevived slot 0x{:X}",
        CanReviveIndex, StopIndex, Rva(CancelAbilities), Rva(UpdateCollision), TimerOffset, OnPawnRevivedIndex);
    Log::Info(L"Finder: DBNO revive: durability penalty slot {}, RefocusQuickBars 0x{:X}, ForgetPawnForAllAI 0x{:X}, stat find 0x{:X} and add 0x{:X}, ServerDBNOReviveInterrupted slot {}",
        DurabilityPenaltyIndex, Rva(RefocusQuickBars), Rva(ForgetPawn), Rva(FindStat), Rva(IncrementStat), Natives::AFortPlayerControllerAthena_ServerDBNOReviveInterruptedIndex);
    return true;
}

int32_t Finder::StartDBNOIndex(ptrdiff_t IsDBNOOffset)
{
    // 21.40's AFortPlayerPawn::StartDBNO, the pawn going down (5.40 AFortPlayerPawnAthena slot 0x1BD, 0xC68250; 5.30
    // slot 0x1B1, 0xC58D50): the death info with bDBNO, then bIsDBNO, the DBNO collision, a gameplay event to the pawn's
    // ability system and, from 5.40, OnPlayerStartDBNO. It is the only one of AFortPlayerPawnAthena's functions that
    // stores 1 to bIsDBNO ("mov byte ptr [rdi+8F0h], 1", 0xC68879); a pawn whose team has nobody standing is finished off
    // there instead (slot 0x1E2), without it.
    void** PawnVTable = CdoVTable(L"/Script/FortniteGame.FortPlayerPawnAthena");
    const int32_t Index = IsDBNOOffset > 0 ? FirstVTableIndexWhere(PawnVTable, [IsDBNOOffset](uintptr_t Entry)
    {
        const uintptr_t End = Finder::FunctionStart(Entry) == Entry ? Finder::FunctionEnd(Entry) : 0;
        for (uintptr_t Site = Entry; End && Site + 7 <= End; ++Site)
        {
            // mov byte ptr [reg+IsDBNOOffset], 1 (C6 /0, disp32, imm8, no SIB).
            const uint8_t ModRm = Byte(Site + 1);
            if (Byte(Site) == 0xC6 && (ModRm & 0xF8) == 0x80 && (ModRm & 0x07) != 0x04 && *reinterpret_cast<const int32_t*>(Site + 2) == IsDBNOOffset && Byte(Site + 6) == 1)
            {
                return true;
            }
        }
        return false;
    }) : -1;
    if (Index < 0)
    {
        Log::Error(L"Finder: AFortPlayerPawn::StartDBNO not found in AFortPlayerPawnAthena (bIsDBNO +0x{:X})", IsDBNOOffset);
        return -1;
    }
    Log::Info(L"Finder: AFortPlayerPawn::StartDBNO is AFortPlayerPawnAthena slot 0x{:X} (0x{:X})", Index, VTableEntry(PawnVTable, Index) - Finder::ImageBase());
    return Index;
}

bool Finder::ResolveDecoSpawnedLive()
{
    static bool bSearched = false;
    if (Natives::AFortPlayerController_DecoSpawnedIndex >= 0 || bSearched)
    {
        return Natives::AFortPlayerController_DecoSpawnedIndex >= 0;
    }
    bSearched = true;

    // AFortDecoTool's spawn (1.7.2 0x91B370, slot 0x124) is its only virtual that initializes what it spawned through
    // ABuildingTrap's InitializeKismetSpawnedBuildingActor slot (0x4A63C0, slot 0xF7, by its log string). It hands the
    // new actor to two of the placer's controller slots, 0x29F before the initialize and 0x2A0 after it, both the shared
    // empty function in 1.7.2: the second is the first virtual call from slot 0x200 up after the initialize.
    const uintptr_t Initialize = FunctionOf(L"Trap %s was loaded from a world record with a missing FortTrapItemDefinition. Destroying next frame.");
    const int32_t InitializeIndex = FirstVTableIndex(CdoVTable(L"/Script/FortniteGame.BuildingTrap"), Initialize);
    const auto FindInitializeCall = [InitializeIndex](uintptr_t Function)
    {
        const uintptr_t FunctionEnd = Finder::FunctionEnd(Function);
        uintptr_t Site = FunctionEnd ? FindVirtualCall(Function, FunctionEnd) : 0;
        while (Site && VirtualSlot(Site) != InitializeIndex)
        {
            Site = FindVirtualCall(Site + 1, FunctionEnd);
        }
        return Site;
    };
    // The spawn is the first DecoTool virtual whose initialize call comes with the controller handoff: the first slot
    // >= 0x200 called after it, the slot before that called before it, and the shared empty function in the controller's
    // vtable. 1.8 has other virtuals (slots 0xD2, 0xFB, 0x11A) that call slot 0xF7 on something else; its spawn is 0x127.
    void** ControllerVTable = CdoVTable(L"/Script/FortniteGame.FortPlayerController");
    const auto FindSpawnedIndex = [&FindInitializeCall, ControllerVTable](uintptr_t Spawn)
    {
        const uintptr_t InitializeCall = FindInitializeCall(Spawn);
        const uintptr_t SpawnEnd = InitializeCall ? Finder::FunctionEnd(Spawn) : 0;
        int32_t SpawnedIndex = -1;
        for (uintptr_t Site = SpawnEnd ? FindVirtualCall(InitializeCall + 1, SpawnEnd) : 0; Site && SpawnedIndex < 0; Site = FindVirtualCall(Site + 1, SpawnEnd))
        {
            SpawnedIndex = VirtualSlot(Site) >= 0x200 ? VirtualSlot(Site) : -1;
        }
        bool bSpawningCalled = false;
        for (uintptr_t Site = SpawnedIndex >= 0 ? FindVirtualCall(Spawn, InitializeCall) : 0; Site && !bSpawningCalled; Site = FindVirtualCall(Site + 1, InitializeCall))
        {
            bSpawningCalled = VirtualSlot(Site) == SpawnedIndex - 1;
        }
        return bSpawningCalled && reinterpret_cast<void*>(VTableEntry(ControllerVTable, SpawnedIndex)) == Natives::SharedEmptyFunction ? SpawnedIndex : -1;
    };
    void** DecoToolVTable = CdoVTable(L"/Script/FortniteGame.FortDecoTool");
    const int32_t SpawnIndex = InitializeIndex >= 0 ? FirstVTableIndexWhere(DecoToolVTable, [&FindSpawnedIndex](uintptr_t Entry) { return FindSpawnedIndex(Entry) >= 0; }) : -1;
    const uintptr_t Spawn = VTableEntry(DecoToolVTable, SpawnIndex);
    const int32_t SpawnedIndex = Spawn ? FindSpawnedIndex(Spawn) : -1;
    if (SpawnedIndex < 0)
    {
        Log::Error(L"Finder: AFortDecoTool's spawn hands the placer's controller no empty slot (initialize slot {}, spawn 0x{:X}, slot {})", InitializeIndex, Spawn ? Spawn - Finder::ImageBase() : 0, SpawnedIndex);
        return false;
    }

    Natives::AFortPlayerController_DecoSpawnedIndex = SpawnedIndex;
    Log::Info(L"Finder: AFortDecoTool's spawn (0x{:X}) hands what it placed to AFortPlayerController slot 0x{:X}", Spawn - Finder::ImageBase(), SpawnedIndex);
    return true;
}

bool Finder::ResolveScoreBuildingConstructionLive()
{
    static bool bSearched = false;
    if (Natives::AFortGameMode_ScoreBuildingConstructionIndex >= 0 || bSearched)
    {
        return Natives::AFortGameMode_ScoreBuildingConstructionIndex >= 0;
    }
    bSearched = true;

    // The game mode scores a player's building in five slots in a row (21.40's order: ScoreBuildingConstruction,
    // Destruction, Upgrade, Repair, Edit). ServerEditBuildingActor_Implementation (1.7.2 0x826410, 1.8 0x846A20) ends with
    // ScoreBuildingEdit (slot 0x133, 1.8 0x132), and ServerCreateBuildingActor_Implementation (0x825810, 1.8 0x845D70)
    // calls ScoreBuildingConstruction, four slots before it, with the controller and the piece it built (0x825E72, 1.8
    // 0x8463D2), after initializing the piece.
    void** ControllerVTable = CdoVTable(L"/Script/FortniteGame.FortPlayerController");
    const auto ImplementationOf = [ControllerVTable](const wchar_t* FunctionPath)
    {
        UFunction* Function = FindObject<UFunction>(nullptr, FunctionPath);
        return Function ? VTableEntry(ControllerVTable, ImplementationIndex(Function)) : 0;
    };
    const uintptr_t Edit = ImplementationOf(L"/Script/FortniteGame.FortPlayerController:ServerEditBuildingActor");
    const uintptr_t EditEnd = Edit ? Finder::FunctionEnd(Edit) : 0;
    int32_t EditIndex = -1;
    for (uintptr_t Site = EditEnd ? FindVirtualCall(Edit, EditEnd) : 0; Site; Site = FindVirtualCall(Site + 1, EditEnd))
    {
        EditIndex = VirtualSlot(Site);
    }
    const int32_t Index = EditIndex >= 4 ? EditIndex - 4 : -1;

    const uintptr_t Create = Index >= 0 ? ImplementationOf(L"/Script/FortniteGame.FortPlayerController:ServerCreateBuildingActor") : 0;
    const uintptr_t CreateEnd = Create ? Finder::FunctionEnd(Create) : 0;
    bool bCreateScores = false;
    for (uintptr_t Site = CreateEnd ? FindVirtualCall(Create, CreateEnd) : 0; Site && !bCreateScores; Site = FindVirtualCall(Site + 1, CreateEnd))
    {
        bCreateScores = VirtualSlot(Site) == Index;
    }
    void** AthenaVTable = CdoVTable(L"/Script/FortniteGame.FortGameModeAthena");
    const uintptr_t Score = bCreateScores ? VTableEntry(AthenaVTable, Index) : 0;
    if (Score && !Finder::IsStubFunction(Score))
    {
        Natives::AFortGameMode_ScoreBuildingConstructionIndex = Index;
        Log::Info(L"Finder: AFortGameMode::ScoreBuildingConstruction is slot 0x{:X} (0x{:X}), called by ServerCreateBuildingActor (0x{:X})", Index, Score - Finder::ImageBase(), Create - Finder::ImageBase());
        return true;
    }

    // 1.8.1 (CL-3729133) strips both: AFortPlayerController's ServerCreateBuildingActor and ServerEditBuildingActor
    // _Implementation slots (0x1F4, 0x1F6) hold the shared empty function. The game mode's own five slots are still there
    // (0x12E-0x132, as on 1.8): Upgrade, Repair and Edit are thunks to one scorer, mov r9, r8; mov r8, rdx; mov dl, Type;
    // jmp Scorer (1.8.1 0x58E940, 0x58E930, 0x58E810, all to 0x58E820; 1.8 0x599300.. to 0x5991E0; 1.7.2 slots 0x131-0x133
    // to 0x58A860), and ScoreBuildingConstruction (1.8.1 0x58E610), two slots before the first of them, calls the scorer
    // too. The first such run is the Athena game mode's own: the subclasses' vtables that follow repeat it. 1.8.2
    // (CL-3741772) strips the RPCs too (0x1F5, 0x1F7) and keeps the run: ScoreBuildingConstruction 0x12E is 0x58F630.
    const auto ScoreThunkTarget = [](uintptr_t Entry) -> uintptr_t
    {
        return Entry && FindInstruction(Entry, Entry + 13, { 0x4D, 0x8B, 0xC8, 0x4C, 0x8B, 0xC2, 0xB2, -1, 0xE9 }) == Entry ? Finder::RelativeTarget(Entry + 8, 1, 5) : 0;
    };
    int32_t ScorerIndex = -1;
    for (int32_t Slot = 2; AthenaVTable && Slot + 2 < 0x400 && ScorerIndex < 0; ++Slot)
    {
        const uintptr_t Upgrade = VTableEntry(AthenaVTable, Slot);
        if (!Upgrade)
        {
            if (IsCoreFunction(reinterpret_cast<uintptr_t>(AthenaVTable[Slot])))
            {
                continue;
            }
            break;
        }
        const uintptr_t Scorer = ScoreThunkTarget(Upgrade);
        if (!Scorer || ScoreThunkTarget(VTableEntry(AthenaVTable, Slot + 1)) != Scorer || ScoreThunkTarget(VTableEntry(AthenaVTable, Slot + 2)) != Scorer)
        {
            continue;
        }
        const uintptr_t Construction = VTableEntry(AthenaVTable, Slot - 2);
        const uintptr_t ConstructionEnd = Construction && Finder::FunctionStart(Construction) == Construction ? Finder::FunctionEnd(Construction) : 0;
        if (ConstructionEnd > Construction && ConstructionEnd - Construction < 0x200 && CallWithin(Construction, ConstructionEnd - Construction, Scorer))
        {
            ScorerIndex = Slot - 2;
        }
    }
    if (ScorerIndex < 0)
    {
        Log::Error(L"Finder: AFortGameMode::ScoreBuildingConstruction not found (edit 0x{:X} scores slot {}, create 0x{:X}, no scorer thunks)",
            Edit ? Edit - Finder::ImageBase() : 0, EditIndex, Create ? Create - Finder::ImageBase() : 0);
        return false;
    }

    Natives::AFortGameMode_ScoreBuildingConstructionIndex = ScorerIndex;
    Log::Info(L"Finder: AFortGameMode::ScoreBuildingConstruction is slot 0x{:X} (0x{:X}), from the game mode's scorer thunks (this build's ServerEditBuildingActor_Implementation is 0x{:X} and scores slot {})",
        ScorerIndex, VTableEntry(AthenaVTable, ScorerIndex) - Finder::ImageBase(), Edit ? Edit - Finder::ImageBase() : 0, EditIndex);
    return true;
}

bool Finder::ResolveGetPoiTagsForLive()
{
    static bool bSearched = false;
    if (Natives::AFortGameMode_GetPoiTagsForIndex >= 0 || bSearched)
    {
        return Natives::AFortGameMode_GetPoiTagsForIndex >= 0;
    }
    bSearched = true;

    // AFortGameMode::GetPoiTagsFor (21.40's name, after IsCVarToGatherPoiTagsFromManagerEnabled) adds the location tags
    // of where an actor is to a tag container. AFortGameModeAthena's (4.0 0xA1EFB0, 3.6 0x9F5B90) asks its POI manager
    // (AFortPoiManager::GetTagsForPawnLocation) for a pawn; AFortGameModeZone's is the shared empty function. The stat
    // senders call it on the game mode just before they send: Interact and Land for the player's pawn, the game mode's
    // Kill for the killer's pawn (source) and the dead pawn (target) (4.0 AFortGameModeZone::Killed 0xC1BBE7 and
    // 0xC1BBFA, [vtable+0xA20], before its Kill at 0xC1BC42). The slot is the one the most senders call through, among
    // those where the Athena game mode has a function and the Zone game mode the empty one: 0x144 on 3.6 and 4.0.
    void** ZoneVTable = CdoVTable(L"/Script/FortniteGame.FortGameModeZone");
    void** AthenaVTable = CdoVTable(L"/Script/FortniteGame.FortGameModeAthena");
    if (!ZoneVTable || !AthenaVTable || !Natives::SharedEmptyFunction || !Finder::ResolveQuestStatEvents() || !Natives::UFortQuestManager_bStatEventsStripped)
    {
        Log::Info(L"Finder: AFortGameMode::GetPoiTagsFor not looked for (no Athena game mode, or the stat events aren't stripped)");
        return false;
    }
    // The team sender's calls count too: GetPoiTagsFor comes before them as before the others (6.00's TeamInteract,
    // after the same interaction's Interact, both under ServerAttemptInteract_Implementation's [vtable+0xA50] at
    // CL-4395664 0x12242F3). Before they were told apart (#402) they were in the typed list, and 6.00's tally was 6 of 14
    // for 0xA50 against 5 for the next slots called there ([vtable+0xA48], and the controller's quest manager getter
    // [vtable+0x1948], which reads past the game modes' vtables); without them the getter's 5 could tie the 5 left.
    std::vector<const uint8_t*> SenderCalls(StrippedStatEventCalls.begin(), StrippedStatEventCalls.end());
    SenderCalls.insert(SenderCalls.end(), StrippedTeamStatEventCalls.begin(), StrippedTeamStatEventCalls.end());
    std::unordered_map<int32_t, int32_t> Senders;
    for (const uint8_t* Call : SenderCalls)
    {
        const uintptr_t Site = reinterpret_cast<uintptr_t>(Call);
        std::vector<int32_t> Slots;
        for (uintptr_t At = FindVirtualCall(Site - 0x90, Site); At; At = FindVirtualCall(At + 1, Site))
        {
            const int32_t Slot = VirtualSlot(At);
            const uintptr_t Athena = VTableEntry(AthenaVTable, Slot);
            if (Athena && !Finder::IsStubFunction(Athena) && reinterpret_cast<void*>(VTableEntry(ZoneVTable, Slot)) == Natives::SharedEmptyFunction
                && std::find(Slots.begin(), Slots.end(), Slot) == Slots.end())
            {
                Slots.push_back(Slot);
                ++Senders[Slot];
            }
        }
    }
    int32_t Index = -1;
    int32_t Most = 0;
    for (const auto& [Slot, Count] : Senders)
    {
        if (Count > Most)
        {
            Index = Slot;
            Most = Count;
        }
    }
    const bool bTied = std::count_if(Senders.begin(), Senders.end(), [Most](const auto& Entry) { return Entry.second == Most; }) > 1;
    if (Index < 0 || bTied)
    {
        Log::Error(L"Finder: AFortGameMode::GetPoiTagsFor not found ({} candidate slots{})", Senders.size(), bTied ? L", tied" : L"");
        return false;
    }

    Natives::AFortGameMode_GetPoiTagsForIndex = Index;
    Log::Info(L"Finder: AFortGameMode::GetPoiTagsFor is slot 0x{:X} (Athena's 0x{:X}), called by {} of the {} stat senders", Index, VTableEntry(AthenaVTable, Index) - Finder::ImageBase(),
        Most, SenderCalls.size());
    return true;
}

uint8_t* Finder::PawnDiedInventoryDropCall()
{
    // AFortPlayerControllerZone's pawn-death handler (1.7.2 0x864E10, slot 0x2F9; the pawn calls it, Athena's override
    // 0x437C30 calls it first) is the only code reading the "Deaths" stat name. Its one call to the shared empty function
    // (0x864F55: the controller, 0, the damage tags, MyFortPawn, &bDroppedBackpack, which then goes into the death report)
    // is the stripped inventory drop. The other "Deaths" reference registers a property.
    void** ZoneVTable = CdoVTable(L"/Script/FortniteGame.FortPlayerControllerZone");
    for (const uintptr_t Ref : Finder::StringRefs(L"Deaths"))
    {
        const uintptr_t Function = Finder::FunctionStart(Ref);
        const int32_t Index = FirstVTableIndex(ZoneVTable, Function);
        const uintptr_t End = Index >= 0 ? Finder::FunctionEnd(Function) : 0;
        std::vector<uintptr_t> StubCalls;
        for (const auto& [Site, Target] : End ? Finder::DirectCalls(Function, End) : std::vector<std::pair<uintptr_t, uintptr_t>>())
        {
            if (reinterpret_cast<void*>(Target) == Natives::SharedEmptyFunction)
            {
                StubCalls.push_back(Site);
            }
        }
        // 2.5 (0xE6A2C0, its body in separate pieces) makes the drop a virtual call, call [rax+1660h] at 0xE6A404, to a
        // slot every controller class fills with the shared empty function.
        if (StubCalls.empty() && Index >= 0)
        {
            for (const auto& [ChunkBegin, ChunkEnd] : Finder::FunctionChunks(Function))
            {
                for (uintptr_t Site = FindVirtualCall(ChunkBegin, ChunkEnd); Site; Site = FindVirtualCall(Site + 1, ChunkEnd))
                {
                    const int32_t Slot = Byte(Site + 1) >= 0x90 && Byte(Site + 1) <= 0x97 ? VirtualSlot(Site) : -1;
                    if (Slot >= 0 && reinterpret_cast<void*>(VTableEntry(ZoneVTable, Slot)) == Natives::SharedEmptyFunction)
                    {
                        StubCalls.push_back(Site);
                    }
                }
            }
        }
        // Not every virtual call there is on the controller: 6.20's (0x12B7450) also asks the killer's pawn for its health
        // and shield for the death report (mov rcx, rbx; call [rax+0C38h], at 0x12B7731), and the controller's own slot
        // 0x187 happens to be the shared empty function. With more than one candidate, keep the calls made on the
        // controller itself: the register the handler copies this into (mov rdi, rcx at 0x12B749C) is moved into rcx
        // right before the call (mov rcx, rdi; call [rax+18F0h] at 0x12B7610, the drop).
        if (StubCalls.size() > 1 && Index >= 0)
        {
            int32_t ThisRegister = -1;
            for (uintptr_t Site = Function; Site + 3 <= End && Site < Function + 0x60 && ThisRegister < 0; ++Site)
            {
                const uint8_t ModRm = Byte(Site + 2);
                if ((Byte(Site) == 0x48 || Byte(Site) == 0x4C) && Byte(Site + 1) == 0x8B && (ModRm & 0xC7) == 0xC1 && (ModRm & 0x38) != 0x08 && (ModRm & 0x38) != 0x20)
                {
                    ThisRegister = ((ModRm >> 3) & 0x07) | (Byte(Site) == 0x4C ? 0x08 : 0x00);
                }
            }
            std::vector<uintptr_t> ControllerCalls;
            for (const uintptr_t Site : ThisRegister >= 0 ? StubCalls : std::vector<uintptr_t>{})
            {
                // mov rcx, ThisRegister: 48 8B C8+r (49 8B C8+r for r8-r15).
                const std::initializer_list<int32_t> MoveThis = { ThisRegister >= 8 ? 0x49 : 0x48, 0x8B, 0xC8 | (ThisRegister & 0x07) };
                if (FindInstruction(Site > 0x10 ? Site - 0x10 : Site, Site, MoveThis))
                {
                    ControllerCalls.push_back(Site);
                }
            }
            if (ControllerCalls.size() == 1)
            {
                StubCalls = ControllerCalls;
            }
        }
        if (StubCalls.size() == 1)
        {
            Log::Info(L"Finder: AFortPlayerControllerZone's pawn-death handler is slot 0x{:X} (0x{:X}), its inventory drop is stripped at 0x{:X}", Index, Function - Finder::ImageBase(), StubCalls[0] - Finder::ImageBase());
            return reinterpret_cast<uint8_t*>(StubCalls[0]);
        }
    }
    Log::Error(L"Finder: the stripped inventory drop in AFortPlayerControllerZone's pawn-death handler was not found, Battle Royale players drop nothing when they die");
    return nullptr;
}

uint8_t* Finder::StartGhostModeGiveItemCall()
{
    // 6.00's AFortPlayerControllerGameplay::StartGhostMode (CL-4395664 0x1268130, the call after its exec's parameter):
    // on the authority it gives the item providing ghost mode to the controller as its IFortInventoryOwnerInterface,
    // UFortKismetLibrary::GiveItemToInventoryOwner({this, interface}, Item, 1, true) (0x1268186), then sets
    // GhostModeRepData (bInGhostMode, GhostModeItemDef) and calls OnRep_GhostModeRepData. GiveItemToInventoryOwner's
    // native is server code: the call goes to the shared empty function its exec (0x14AB400) also ends in, and it is the
    // body's only direct call to a stub. Older builds have no StartGhostMode.
    UFunction* StartGhostMode = FindObject<UFunction>(nullptr, L"/Script/FortniteGame.FortPlayerControllerGameplay:StartGhostMode");
    if (!StartGhostMode)
    {
        return nullptr;
    }
    const uintptr_t ExecCall = reinterpret_cast<uintptr_t>(Finder::ImplementationCall(StartGhostMode));
    const uintptr_t Native = ExecCall ? Finder::RelativeTarget(ExecCall, 1, 5) : 0;
    const uintptr_t NativeEnd = Native && Finder::FunctionStart(Native) == Native ? Finder::FunctionEnd(Native) : 0;
    std::vector<uintptr_t> StubCalls;
    for (const auto& [Site, Target] : NativeEnd ? Finder::DirectCalls(Native, NativeEnd) : std::vector<std::pair<uintptr_t, uintptr_t>>())
    {
        if (Finder::IsStubFunction(Target))
        {
            StubCalls.push_back(Site);
        }
    }
    if (StubCalls.size() != 1)
    {
        Log::Error(L"Finder: AFortPlayerControllerGameplay::StartGhostMode's stripped GiveItemToInventoryOwner was not found (native {}, {} stub calls), a Shadow Stone gives no shadow form",
            Native != 0, StubCalls.size());
        return nullptr;
    }
    Log::Info(L"Finder: AFortPlayerControllerGameplay::StartGhostMode (0x{:X}) gives its item with the stripped GiveItemToInventoryOwner at 0x{:X}", Native - Finder::ImageBase(),
        StubCalls[0] - Finder::ImageBase());
    return reinterpret_cast<uint8_t*>(StubCalls[0]);
}

uint8_t* Finder::DiscoPawnDeathSpawnPickupCall(UFunction* SpawnPickupInWorld)
{
    // 6.01's Disco Domination. While its players respawn, a dead one leaves a share of each ammo stack behind:
    // AFortAthenaMutator_Disco's AFortGameModeAthena_OnPawnDeathBeforeRespawning (21.40's name for the mutators'
    // virtual; 6.01 CL-4424678 0xC6CE00, 6.02 CL-4461277 0xC6F790, slot 0xC5), which the pawn's death reaches through
    // the game mode's walk of its mutators (6.02 0xC48A10 -> 0xBD3820). For every ammo item of the dead player's
    // inventory it takes the count times CachedPercentAmmoToDrop ("mulss xmm0, [r14+490h]") and calls
    // UFortKismetLibrary::K2_SpawnPickupInWorld(GetWorld(), ItemDefinition, Count, PawnLocation, Direction, 0, true,
    // true, false) (6.02 0xC6FA6B). That native is server code: the call goes to the return-null stub its exec (6.02
    // 0x14A73A6) calls with the same nine parameters, and it is the only call to that stub in the mutator's own
    // functions. Builds whose Disco mutator has no CachedPercentAmmoToDrop (6.00) or no Disco mutator have none.
    UClass* DiscoClass = FindObject<UClass>(nullptr, L"/Script/FortniteGame.FortAthenaMutator_Disco");
    const UProperty* PercentProperty = DiscoClass ? DiscoClass->FindPropertyByName(FName(L"CachedPercentAmmoToDrop", FNAME_Find)) : nullptr;
    if (!PercentProperty)
    {
        return nullptr;
    }
    void** DiscoVTable = CdoVTable(L"/Script/FortniteGame.FortAthenaMutator_Disco");
    void** MutatorVTable = CdoVTable(L"/Script/FortniteGame.FortAthenaMutator");
    const uintptr_t ExecCall = reinterpret_cast<uintptr_t>(Finder::ImplementationCall(SpawnPickupInWorld));
    const uintptr_t Stub = ExecCall ? Finder::RelativeTarget(ExecCall, 1, 5) : 0;
    const int32_t Percent = PercentProperty->GetOffset_ForInternal();
    std::vector<uintptr_t> StubCalls;
    uintptr_t Function = 0;
    for (int32_t Slot = 0; DiscoVTable && MutatorVTable && Finder::IsStubFunction(Stub) && Slot < 0x200; ++Slot)
    {
        const uintptr_t Own = reinterpret_cast<uintptr_t>(DiscoVTable[Slot]);
        const uintptr_t Base = reinterpret_cast<uintptr_t>(MutatorVTable[Slot]);
        if (!IsInText(Own) || !IsInText(Base))
        {
            break;
        }
        // The mutator's own overrides, each looked at once.
        const uintptr_t OwnEnd = Own != Base && Own != Function && Finder::FunctionStart(Own) == Own ? Finder::FunctionEnd(Own) : 0;
        if (OwnEnd <= Own || !FindInstruction(Own, OwnEnd, { Percent & 0xFF, (Percent >> 8) & 0xFF, (Percent >> 16) & 0xFF, (Percent >> 24) & 0xFF }))
        {
            continue;
        }
        for (const auto& [Site, Target] : Finder::DirectCalls(Own, OwnEnd))
        {
            if (Target == Stub)
            {
                StubCalls.push_back(Site);
                Function = Own;
            }
        }
    }
    if (StubCalls.size() != 1)
    {
        Log::Error(L"Finder: AFortAthenaMutator_Disco's stripped K2_SpawnPickupInWorld was not found (the exec's stub {}, {} calls to it in the mutator's functions that read CachedPercentAmmoToDrop +0x{:X}), Disco Domination's dead leave no ammo",
            Finder::IsStubFunction(Stub) ? L"found" : L"not found", StubCalls.size(), Percent);
        return nullptr;
    }
    Log::Info(L"Finder: AFortAthenaMutator_Disco::AFortGameModeAthena_OnPawnDeathBeforeRespawning (0x{:X}) drops its ammo with the stripped K2_SpawnPickupInWorld at 0x{:X}",
        Function - Finder::ImageBase(), StubCalls[0] - Finder::ImageBase());
    return reinterpret_cast<uint8_t*>(StubCalls[0]);
}

bool Finder::ResolveServerMoveLive()
{
    static bool bSearched = false;
    if (Natives::UCharacterMovementComponent_ServerMove_ImplementationIndex >= 0 || bSearched)
    {
        return Natives::UCharacterMovementComponent_ServerMove_ImplementationIndex >= 0;
    }
    bSearched = true;

    // ACharacter::ServerMove's exec calls ServerMove_Validate, then ACharacter::ServerMove_Implementation (1.7.2 0x1FE7F80,
    // its last call), whose only virtual call passes the move to the movement component's ServerMove_Implementation (slot
    // 0x128, 0x20222E0). ServerMoveDual's moves go through the same slot.
    UFunction* ServerMove = FindObject<UFunction>(nullptr, L"/Script/Engine.Character:ServerMove");
    const uintptr_t Exec = ServerMove ? reinterpret_cast<uintptr_t>(ServerMove->GetNativeFunc()) : 0;
    const uintptr_t ExecEnd = IsInText(Exec) ? Finder::FunctionEnd(Exec) : 0;
    const std::vector<std::pair<uintptr_t, uintptr_t>> Calls = ExecEnd > Exec ? Finder::DirectCalls(Exec, ExecEnd) : std::vector<std::pair<uintptr_t, uintptr_t>>();
    const uintptr_t Implementation = Calls.empty() ? 0 : Calls.back().second;
    const uintptr_t ImplementationEnd = Implementation && Finder::FunctionStart(Implementation) == Implementation ? Finder::FunctionEnd(Implementation) : 0;
    const uintptr_t Call = ImplementationEnd ? FindVirtualCall(Implementation, ImplementationEnd) : 0;
    const int32_t Index = VirtualSlot(Call);
    if (Index < 0 || FindVirtualCall(Call + 1, ImplementationEnd))
    {
        Log::Error(L"Finder: UCharacterMovementComponent::ServerMove_Implementation slot not found");
        return false;
    }

    Natives::UCharacterMovementComponent_ServerMove_ImplementationIndex = Index;
    Log::Info(L"Finder: UCharacterMovementComponent::ServerMove_Implementation is slot 0x{:X}", Index);
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
            if (Finder::BodyIs(InnerTarget, { 0xB0, 0x01, 0xC3 }))
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

// AFortWorldManager::SpawnExplorationActors has no strings. It opens by copying ExplorationActorDistanceRangeMin and Max into
// its FFortPlacementActorFinderInfo: movss xmm0, [rcx+Min] then movss xmm1, [rcx+Max] (1.2 has an xor between them; 1.2
// 0x140594F90, 1.7.2 0x61910F, 1.8 0x64AC9F). The placement finder it calls with that info is stripped to the empty-TArray
// return xor eax,eax / mov [rcx],rax / mov [rcx+8],rax / mov rax,rcx / ret.
uint8_t* Finder::ExplorationActorFinderCall(ptrdiff_t DistanceRangeMinOffset, ptrdiff_t DistanceRangeMaxOffset)
{
    const int32_t Min = static_cast<int32_t>(DistanceRangeMinOffset);
    const int32_t Max = static_cast<int32_t>(DistanceRangeMaxOffset);
    const uint8_t MinLoad[] = { 0xF3, 0x0F, 0x10, 0x81, static_cast<uint8_t>(Min), static_cast<uint8_t>(Min >> 8), static_cast<uint8_t>(Min >> 16), static_cast<uint8_t>(Min >> 24) };
    const uint8_t* const TextFirst = reinterpret_cast<const uint8_t*>(Finder::TextBegin());
    const uint8_t* const TextLast = reinterpret_cast<const uint8_t*>(Finder::TextEnd());
    uintptr_t Spawn = 0;
    int32_t Functions = 0;
    for (const uint8_t* At = std::search(TextFirst, TextLast, std::begin(MinLoad), std::end(MinLoad)); At != TextLast; At = std::search(At + 1, TextLast, std::begin(MinLoad), std::end(MinLoad)))
    {
        const uintptr_t Site = reinterpret_cast<uintptr_t>(At);
        const uintptr_t Start = FindInstruction(Site + sizeof(MinLoad), Site + 0x18, { 0xF3, 0x0F, 0x10, 0x89, Max & 0xFF, (Max >> 8) & 0xFF, (Max >> 16) & 0xFF, (Max >> 24) & 0xFF }) ? Finder::FunctionStart(Site) : 0;
        if (Start && Site - Start < 0x20 && Start != Spawn)
        {
            Spawn = Start;
            ++Functions;
        }
    }

    static const uint8_t EmptyArrayReturn[] = { 0x33, 0xC0, 0x48, 0x89, 0x01, 0x48, 0x89, 0x41, 0x08, 0x48, 0x8B, 0xC1, 0xC3 };
    uintptr_t Found = 0;
    int32_t Count = 0;
    for (const auto& [Call, Target] : Functions == 1 ? Finder::DirectCalls(Spawn, Finder::FunctionEnd(Spawn)) : decltype(Finder::DirectCalls(0, 0)){})
    {
        if (std::memcmp(reinterpret_cast<const void*>(Target), EmptyArrayReturn, sizeof(EmptyArrayReturn)) == 0)
        {
            Found = Call;
            ++Count;
        }
    }

    if (Count != 1)
    {
        Log::Error(L"Finder: AFortWorldManager::SpawnExplorationActors placement finder call not found ({} functions, {} calls)", Functions, Count);
        return nullptr;
    }
    Log::Info(L"Finder: AFortWorldManager::SpawnExplorationActors +0x{:X}, placement finder call at 0x{:X}", Spawn - Finder::ImageBase(), Found - Finder::ImageBase());
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

    // Slots Core has already swapped are stepped over, as in FirstVTableIndexWhere: ACharacter::HookCheatMovement swaps
    // ClientCheatWalk/Fly/Ghost_Implementation in every character's vtable at boot (1.8 slots 0x104-0x106, 1.7.2
    // 0x108-0x10A), and a walk that stopped there never reached AFortPawn::EquipDefaultWeapon (1.8 slot 0x133 = 0x7D9370,
    // 1.7.2 0x137 = 0x783300) or AFortPlayerPawn::EquipLastWeapon (0x18E: 1.8 0x7D93A0, 1.7.2 0x7CA430). Both came back -1,
    // so the pawn kept no weapon when an ability weapon such as Goin' Commando's minigun went away (#115).
    int32_t UniqueSlotReferencing(void** VTable, size_t MaxSize, std::initializer_list<ptrdiff_t> Offsets, bool bNeedsCall)
    {
        int32_t Found = -1;
        uintptr_t FoundFunction = 0;
        for (int32_t Index = 0; VTable && Index < 0x200; ++Index)
        {
            const uintptr_t Function = VTableEntry(VTable, Index);
            if (!Function)
            {
                if (IsCoreFunction(reinterpret_cast<uintptr_t>(VTable[Index])))
                {
                    continue;
                }
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
        if (Finder::BodyIs(Target, { 0x33, 0xC0, 0xC3 }))
        {
            ContextCall = ContextCall ? UINTPTR_MAX : Call;
        }
        else if (Finder::BodyIs(Target, { 0x32, 0xC0, 0xC3 }))
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

bool Finder::ResolveSetHomeActorLive(ptrdiff_t HomeActorOffset)
{
    if (Natives::AFortPlayerState_SetHomeActor)
    {
        return true;
    }

    // AFortGameMode::SpawnDefaultPawnFor_Implementation binds the start spot as the player's home actor when they have
    // none (6.02 0xE5CAFD: call the getter, mov rax, [rcx+8C8h]; ret; then 0xE5CB0D: call the setter with the player
    // state and the start spot). The setter is the one callee that reads the old home actor and writes the new one
    // (6.02 0x1244330, 6.10 0x128B260, 6.20 0x130D950: mov r64, [r64+HomeActor] ... mov [r64+HomeActor], r64); the
    // getter only reads it.
    const auto CountAccesses = [HomeActorOffset](uintptr_t Begin, uintptr_t End, uint8_t Opcode)
    {
        // REX.W mov with a [base+disp32] operand: not a SIB form (rm 100) and not off rbp (rm 101 without REX.B).
        int32_t Count = 0;
        for (uintptr_t Site = Begin; Site + 7 <= End; ++Site)
        {
            const uint8_t Rex = Byte(Site);
            const uint8_t ModRM = Byte(Site + 2);
            const bool bRbp = (ModRM & 0x07) == 0x05 && (Rex & 0x01) == 0;
            if ((Rex & 0xF8) == 0x48 && Byte(Site + 1) == Opcode && (ModRM & 0xC0) == 0x80 && (ModRM & 0x07) != 0x04 && !bRbp
                && *reinterpret_cast<const int32_t*>(Site + 3) == static_cast<int32_t>(HomeActorOffset))
            {
                ++Count;
            }
        }
        return Count;
    };

    const uintptr_t SpawnPawn = reinterpret_cast<uintptr_t>(Natives::AFortGameMode_SpawnDefaultPawnFor);
    uintptr_t SetHomeActor = 0;
    bool bAmbiguous = false;
    for (const auto& [ChunkBegin, ChunkEnd] : SpawnPawn && HomeActorOffset > 0 ? Finder::FunctionChunks(SpawnPawn) : std::vector<std::pair<uintptr_t, uintptr_t>>())
    {
        for (const auto& [Call, Target] : Finder::DirectCalls(ChunkBegin, ChunkEnd))
        {
            int32_t Loads = 0;
            int32_t Stores = 0;
            size_t Size = 0;
            for (const auto& [TargetBegin, TargetEnd] : Finder::FunctionStart(Target) == Target ? Finder::FunctionChunks(Target) : std::vector<std::pair<uintptr_t, uintptr_t>>())
            {
                Size += TargetEnd - TargetBegin;
                Loads += Size <= 0x400 ? CountAccesses(TargetBegin, TargetEnd, 0x8B) : 0;
                Stores += Size <= 0x400 ? CountAccesses(TargetBegin, TargetEnd, 0x89) : 0;
            }
            if (Size <= 0x400 && Loads > 0 && Stores > 0)
            {
                bAmbiguous |= SetHomeActor && SetHomeActor != Target;
                SetHomeActor = Target;
            }
        }
    }

    if (!SetHomeActor || bAmbiguous)
    {
        Log::Error(L"Finder: AFortPlayerState's home actor setter not found in AFortGameMode::SpawnDefaultPawnFor_Implementation{}", bAmbiguous ? L" (more than one candidate)" : L"");
        return false;
    }
    Natives::AFortPlayerState_SetHomeActor = reinterpret_cast<decltype(Natives::AFortPlayerState_SetHomeActor)>(SetHomeActor);
    Log::Info(L"Finder: AFortPlayerState home actor setter at 0x{:X} (HomeActor +0x{:X})", SetHomeActor - Finder::ImageBase(), HomeActorOffset);
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

    void** WeaponVTable = CdoVTable(L"/Script/FortniteGame.FortWeapon");
    int32_t Index = UniqueSlotReferencing(WeaponVTable, 0x80, { AmmoCountOffset, ItemEntryGuidOffset }, true);
    // 1.9's (CL-3757339, slot 0xF0, 0x94B3F0) is 0x8B bytes (1.8.2's 0x9379A0 is 0x7F); the only other weapon virtual
    // reading both members is 0x1DB bytes (slot 0xE0).
    if (Index < 0)
    {
        Index = UniqueSlotReferencing(WeaponVTable, 0x100, { AmmoCountOffset, ItemEntryGuidOffset }, true);
    }
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
            const bool bLoadsDurability = Byte(Site) == 0xF3 && Byte(Site + 1) == 0x0F && Byte(Site + 2) == 0x10 && (ModRm & 0xF8) == 0x48 && ModRm != 0x4C && Byte(Site + 4) == EntryDurabilityOffset;
            // 1.8 reads it through FFortItemEntry's getter (0x3F1F440: movss xmm0, [rcx+Durability]; ret), then movaps xmm1, xmm0.
            const uintptr_t Getter = Byte(Site) == 0xE8 ? Finder::RelativeTarget(Site, 1, 5) : 0;
            const bool bCallsGetter = IsInText(Getter) && Byte(Getter) == 0xF3 && Byte(Getter + 1) == 0x0F && Byte(Getter + 2) == 0x10 && Byte(Getter + 3) == 0x41 && Byte(Getter + 4) == EntryDurabilityOffset && Byte(Getter + 5) == 0xC3;
            if (!bLoadsDurability && !bCallsGetter)
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
    const bool bSetDurabilityStripped = Finder::BodyIs(SetDurability, { 0x32, 0xC0, 0xC3 }) || Finder::BodyIs(SetDurability, { 0x33, 0xC0, 0xC3 });

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
    // 1.9 (CL-3757339, slot 0xC6, 0x93AB70) sets the flag first and tail-jumps to the destroy it used to call through
    // the vtable (0x94E620, now OnRep_EquippedWeaponDestroyWrapper's body): or dword [rcx+9E0h], 8;
    // cmp byte [rcx+118h], 3; jnz; inc byte [rcx+3E8h]; jmp.
    const bool bDestroyedFound = DestroyedIndex >= 0;
    for (int32_t Index = 0; !bDestroyedFound && Index < 0x200; ++Index)
    {
        const uintptr_t Function = VTableEntry(WeaponVTable, Index);
        if (!Function)
        {
            break;
        }
        if (Function == DestroyedFunction || (Byte(Function) != 0x83 && Byte(Function) != 0x80) || Byte(Function + 1) != 0x89
            || *reinterpret_cast<const int32_t*>(Function + 2) != PendingDestroyDueToDurabilityOffset || Byte(Function + 6) != PendingDestroyDueToDurabilityMask)
        {
            continue;
        }
        for (uintptr_t Jump = Function + 7; Jump + 5 <= Function + 0x20; ++Jump)
        {
            const uintptr_t Target = Byte(Jump) == 0xE9 ? Finder::RelativeTarget(Jump, 1, 5) : 0;
            if (IsInText(Target) && Finder::FunctionStart(Target) == Target)
            {
                bDestroyedAmbiguous |= DestroyedFunction != 0;
                DestroyedIndex = Index;
                DestroyedFunction = Function;
                break;
            }
        }
    }

    // 5.0 (UE 4.21) keeps the destroy in the function (CL-4214610 0x12978C0): after its prologue, or dword ptr
    // [rcx+0A00h], 10h; mov rbx, rcx; cmp byte ptr [rcx+118h], 3 (ROLE_Authority); jnz; inc byte ptr [rcx+3BCh]; ...
    for (int32_t Index = 0; DestroyedIndex < 0 && Index < 0x200; ++Index)
    {
        const uintptr_t Function = VTableEntry(WeaponVTable, Index);
        if (!Function)
        {
            break;
        }
        for (uintptr_t Site = Function; Site + 7 <= Function + 0x14; ++Site)
        {
            if ((Byte(Site) == 0x83 || Byte(Site) == 0x80) && Byte(Site + 1) == 0x89 && *reinterpret_cast<const int32_t*>(Site + 2) == PendingDestroyDueToDurabilityOffset
                && Byte(Site + 6) == PendingDestroyDueToDurabilityMask && FindInstruction(Site + 7, Site + 0x14, { 0x80, 0xB9, -1, -1, 0x00, 0x00, 0x03 }))
            {
                bDestroyedAmbiguous |= DestroyedFunction != 0 && DestroyedFunction != Function;
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
    // Set together on success (4.2 leaves FDelegateBase_Allocate null, see below).
    if (Natives::McpRequestDelegateInstanceVTable && Natives::AFortPlayerControllerOutpost_GetFabricatorProgress)
    {
        return true;
    }

    const uintptr_t Abort = FunctionOf(L"AFortPlayerControllerOutpost::ServerAbortDisintegration");
    const uintptr_t AbortEnd = Abort ? Finder::FunctionEnd(Abort) : 0;
    // The context's fields are frame slots: [rbp+disp8] up to 4.1, [rsp+disp8] on 4.2, whose ServerAbortDisintegration
    // (0xF798F0) keeps no frame pointer. ModRmSite is the ModRM byte; the result is the displacement byte's distance from
    // it (1 for rbp, 2 past the rsp SIB byte 24), 0 when it isn't that form.
    const auto FrameSlot = [](uintptr_t ModRmSite, uint8_t Register, bool bRsp) -> uintptr_t
    {
        const uint8_t ModRm = Byte(ModRmSite);
        if (!bRsp)
        {
            return ModRm == (0x45 | (Register << 3)) ? 1 : 0;
        }
        return ModRm == (0x44 | (Register << 3)) && Byte(ModRmSite + 1) == 0x24 ? 2 : 0;
    };
    uintptr_t Completed = 0;
    const uintptr_t VTable = OutpostMcpRequestVTable(Abort, AbortEnd, Completed);

    const uintptr_t HandleStore = Abort ? FindInstruction(Abort, AbortEnd, { 0x0F, 0x11, -1, 0x10, 0xE8 }) : 0;
    uintptr_t UrlLoad = 0;
    for (uintptr_t Reference : Finder::StringRefs(L"McpDedicatedServerCommandUrl"))
    {
        UrlLoad = Reference >= Abort && Reference < AbortEnd ? Reference : UrlLoad;
    }

    // Copy, UrlName, Credentials and Context hold the displacement byte of their frame slot; the rbp form is tried
    // first, so builds up to 4.1 decode exactly as before.
    uintptr_t Copy = 0;
    uintptr_t CopySlot = 0;
    uintptr_t UrlName = 0;
    uintptr_t Credentials = 0;
    uintptr_t Context = 0;
    for (const bool bRsp : { false, true })
    {
        Copy = CopySlot = UrlName = Credentials = Context = 0;
        // The delegate is copied into the context: lea rdx, [frame+Delegate]; call [rax+CreateCopy]. 4.2 sets this
        // between them (0xF799C6: lea rdx, [rsp+40h]; mov rcx, rbx; call [rax+30h]).
        for (uintptr_t Site = Abort; Site && !Copy && Site + 10 <= AbortEnd; ++Site)
        {
            const uintptr_t Slot = Byte(Site) == 0x48 && Byte(Site + 1) == 0x8D ? FrameSlot(Site + 2, 2, bRsp) : 0;
            const uintptr_t Next = Site + 3 + Slot;
            if (Slot && Byte(Next) == 0xFF && Byte(Next + 1) == 0x50)
            {
                Copy = Site + 2 + Slot;
                CopySlot = Next + 2;
            }
            else if (Slot && bRsp && Byte(Next) == 0x48 && Byte(Next + 1) == 0x8B && Byte(Next + 3) == 0xFF && Byte(Next + 4) == 0x50)
            {
                Copy = Site + 2 + Slot;
                CopySlot = Next + 5;
            }
        }
        // lea rcx, [frame+UrlConfigName] for the FString the url name is copied into.
        for (uintptr_t Site = UrlLoad ? UrlLoad - 0x30 : 0; Site && Site < UrlLoad; ++Site)
        {
            const uintptr_t Slot = Byte(Site) == 0x48 && Byte(Site + 1) == 0x8D ? FrameSlot(Site + 2, 1, bRsp) : 0;
            UrlName = Slot ? Site + 2 + Slot : UrlName;
        }
        // mov dword [frame+ContextCredentials], 1
        for (uintptr_t Site = UrlLoad; Site && !Credentials && Site + 8 <= AbortEnd; ++Site)
        {
            const uintptr_t Slot = Byte(Site) == 0xC7 ? FrameSlot(Site + 1, 0, bRsp) : 0;
            Credentials = Slot && *reinterpret_cast<const int32_t*>(Site + 2 + Slot) == 1 ? Site + 1 + Slot : 0;
        }
        // The request is made from the context: lea rdx, [frame+Context]; mov rcx, <profile>; call. 1.10 and 1.11 keep
        // the profile in r14 (mov rcx, r14: 49 8B CE; 1.10 0x8DFF8B, 1.11 0x8F2EDB), earlier builds in a low register
        // (48 8B; 4.2 0xF79A49: lea rdx, [rsp+40h]; mov rcx, rdi).
        for (uintptr_t Site = Credentials; Site && Site + 10 <= AbortEnd; ++Site)
        {
            const uintptr_t Slot = Byte(Site) == 0x48 && Byte(Site + 1) == 0x8D ? FrameSlot(Site + 2, 2, bRsp) : 0;
            const uintptr_t Next = Site + 3 + Slot;
            Context = Slot && (Byte(Next) == 0x48 || Byte(Next) == 0x49) && Byte(Next + 1) == 0x8B && Byte(Next + 3) == 0xE8 ? Site + 2 + Slot : Context;
        }
        if (Copy && UrlName && Credentials && Context)
        {
            break;
        }
    }

    // The context's FDelegateBase runs from its delegate to its url name: 0x40 bytes up to 4.1 (TInlineAllocator<2>),
    // 0x10 on 4.2 (UE 4.21's NUM_DELEGATE_INLINE_BYTES 0: FHeapAllocator Data and DelegateSize, FDelegateBase.h). The
    // server never runs the client's delegate-layout finder, so the layout is read from the context here. Up to 4.1
    // CreateCopy calls FDelegateBase::Allocate first; 4.2 inlines it (0x29824B0), and FDedicatedServerUrlContext::Bind
    // allocates as it does.
    const bool bHeapDelegates = Copy && UrlName && static_cast<int8_t>(Byte(UrlName)) - static_cast<int8_t>(Byte(Copy)) == 0x10;
    const uintptr_t CreateCopy = VTable && Copy ? VTableEntry(reinterpret_cast<void**>(VTable), Byte(CopySlot) / 8) : 0;
    const uintptr_t AllocateCall = CreateCopy && !bHeapDelegates ? FindRawCall(CreateCopy, CreateCopy + 0x40) : 0;
    const uintptr_t CompletedEnd = Completed ? Finder::FunctionEnd(Completed) : 0;
    const auto CompletedCalls = CompletedEnd ? Finder::DirectCalls(Completed, CompletedEnd) : std::vector<std::pair<uintptr_t, uintptr_t>>();
    if (!Completed || !VTable || !HandleStore || !Copy || !UrlName || !Credentials || !Context || (!AllocateCall && !bHeapDelegates) || CompletedCalls.size() < 3)
    {
        Log::Error(L"Finder: outpost MCP request not decoded (completed {}, vtable {}, handle {}, copy {}, url {}, credentials {}, context {}, allocate {}, completed calls {})",
            Completed != 0, VTable != 0, HandleStore != 0, Copy != 0, UrlName != 0, Credentials != 0, Context != 0, AllocateCall != 0, CompletedCalls.size());
        return false;
    }

    const int32_t Base = static_cast<int8_t>(Byte(Context));
    Natives::FBaseUrlContext_Delegate = static_cast<int8_t>(Byte(Copy)) - Base;
    Natives::FBaseUrlContext_UrlConfigName = static_cast<int8_t>(Byte(UrlName)) - Base;
    Natives::FBaseUrlContext_ContextCredentials = static_cast<int8_t>(Byte(Credentials)) - Base;
    Natives::McpRequestDelegateInstanceVTable = reinterpret_cast<void*>(VTable);
    Natives::FDelegateBase_Allocate = AllocateCall ? reinterpret_cast<decltype(Natives::FDelegateBase_Allocate)>(Finder::RelativeTarget(AllocateCall, 1, 5)) : nullptr;
    Natives::FBaseUrlContext_bHeapDelegate = bHeapDelegates;
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
    return Finder::BodyIs(Function, { 0xC3 })
        || Finder::BodyIs(Function, { 0xC2, 0x00, 0x00 })
        || Finder::BodyIs(Function, { 0x32, 0xC0, 0xC3 })
        || Finder::BodyIs(Function, { 0x33, 0xC0, 0xC3 })
        || Finder::BodyIs(Function, { 0xB0, 0x01, 0xC3 })
        || Finder::BodyIs(Function, { 0x0F, 0x57, 0xC0, 0xC3 });
}

// A stripped body that still calls UObject::GetWorld and drops the result, and otherwise only calls Super (or nothing).
// 2.4.2 leaves these: ABuildingContainer::SpawnLoot is `if (Role == ROLE_Authority) GetWorld(); return false;`
// (0x4B29A0) and its PostUpdate calls the stripped ABuildingSMActor::PostUpdate (0x4AA8B0), then tail-jumps to
// GetWorld (0x4AA840).
bool Finder::IsGetWorldStub(uintptr_t Function, uintptr_t Super)
{
    if (!IsInText(Function) || (Natives::UObject_GetWorldIndex < 0 && !Finder::ResolveObjectGetWorld()))
    {
        return false;
    }
    const uintptr_t End = Finder::FunctionEnd(Function);
    if (!End || End <= Function || End - Function > 0x40)
    {
        return false;
    }
    const int32_t GetWorld = Natives::UObject_GetWorldIndex * static_cast<int32_t>(sizeof(void*));
    int32_t GetWorldCalls = 0;
    for (uintptr_t Address = Function; Address + 6 <= End; ++Address)
    {
        const uint8_t ModRm = Byte(Address + 1);
        if (Byte(Address) == 0xFF && ((ModRm & 0xF8) == 0x90 || (ModRm & 0xF8) == 0xA0) && (ModRm & 0x07) != 0x04)
        {
            if (*reinterpret_cast<const int32_t*>(Address + 2) != GetWorld)
            {
                return false;
            }
            ++GetWorldCalls;
        }
    }
    for (uintptr_t Call = FindRawCall(Function, End); Call; Call = FindRawCall(Call + 5, End))
    {
        if (!Super || Finder::RelativeTarget(Call, 1, 5) != Super)
        {
            return false;
        }
    }
    return GetWorldCalls == 1;
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

    // 5.0 (UE 4.21) inlines GetItemLevel(LootLevelData, WorldLevel) into a caller that takes a world context in rdx and
    // works out the world level itself (CL-4214610 0xF33D40: test rdx, rdx; jz; mov rax, [rdx]; call [rax+140h]
    // (GetWorld), then the handle at [rcx+378h]); that one can't be called with a handle, so loot gets item level 0, as
    // Core gives without the native (Battle Royale items have no levels).
    const uintptr_t ContextTest = GetItemLevel ? FindInstruction(GetItemLevel, GetItemLevel + 0x30, { 0x48, 0x85, 0xD2 }) : 0;
    const bool bTakesWorldContext = ContextTest && FindInstruction(ContextTest + 3, ContextTest + 0x10, { 0x48, 0x8B, 0x02 });
    // Each is kept on its own, so the item levels don't depend on the tag slot.
    if (GetItemLevel && !bTakesWorldContext)
    {
        Natives::UFortLootLevel_GetItemLevel = reinterpret_cast<decltype(Natives::UFortLootLevel_GetItemLevel)>(GetItemLevel);
    }
    if (bTakesWorldContext)
    {
        Log::Info(L"Finder: UFortLootLevel::GetItemLevel is inlined into a world-context caller (+0x{:X}) in this build, loot items get level 0", GetItemLevel - Finder::ImageBase());
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

    // ABuildingSMActor::SelectMeshSet(int32 LootTier) survives in 1.2 and ends in the SetMeshSet virtual. The byte scan
    // also meets FF 9x in other instructions: 4.5's last log string lea (CL-4166199 0xB282DB) reads as a call with a
    // displacement no slot has, after the real calls ([rax+0D20h], [r8+0D20h]), so only a slot counts.
    const uintptr_t SelectMeshSet = FunctionOf(L"ABuildingSMActor::SelectMeshSet(int32 LootTier) just set AltMeshIdx Building: %s, AltMeshIdx: %d");
    const uintptr_t SelectMeshSetEnd = SelectMeshSet ? Finder::FunctionEnd(SelectMeshSet) : 0;
    int32_t SetMeshSetIndex = -1;
    for (uintptr_t Site = SelectMeshSetEnd ? FindVirtualCall(SelectMeshSet, SelectMeshSetEnd) : 0; Site; Site = FindVirtualCall(Site + 1, SelectMeshSetEnd))
    {
        SetMeshSetIndex = VirtualSlot(Site) >= 0 ? VirtualSlot(Site) : SetMeshSetIndex;
    }

    // ABuildingSMActor::PostUpdate: Cert selects the alternate mesh set in it, 1.2 only logs.
    // 5.30 (CL-4305896) logs the line from a second function (0xCC4CA0, which goes on to call 0xCC71D0) ahead of the
    // stripped PostUpdate (0xCC4D50), and the vtable scan runs on into it at slot 0x319: the earliest slot wins.
    constexpr const wchar_t* PostUpdateText = L"ABuildingSMActor::PostUpdate() Building: %s, AltMeshIdx: %d";
    void** SMActorVTable = CdoVTable(L"/Script/FortniteGame.BuildingSMActor");
    uintptr_t PostUpdate = FunctionOf(PostUpdateText);
    int32_t PostUpdateIndex = FirstVTableIndex(SMActorVTable, PostUpdate);
    for (const uintptr_t Ref : Finder::StringRefs(PostUpdateText))
    {
        const uintptr_t Function = Finder::FunctionStart(Ref);
        const int32_t Index = FirstVTableIndex(SMActorVTable, Function);
        if (Index >= 0 && (PostUpdateIndex < 0 || Index < PostUpdateIndex))
        {
            PostUpdate = Function;
            PostUpdateIndex = Index;
        }
    }
    bool bPostUpdateSelectsMeshSet = false;
    for (const auto& [Site, Target] : Calls(PostUpdate))
    {
        bPostUpdateSelectsMeshSet |= Target == SelectMeshSet;
    }

    // SpawnLoot: the last virtual call in the container's IFortInteractInterface::ServerOnAttemptInteract, its only one up
    // to 2.5. 3.0 (0xA3E4F0) first asks another virtual (slot 0x1A1) whether the pawn may search, then calls SpawnLoot.
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
    const uintptr_t FirstInteractCall = AttemptInteractEnd ? FindVirtualCall(AttemptInteract, AttemptInteractEnd) : 0;
    uintptr_t SpawnLootCall = FirstInteractCall;
    for (uintptr_t Site = SpawnLootCall; Site; Site = FindVirtualCall(Site + 1, AttemptInteractEnd))
    {
        SpawnLootCall = Site;
    }
    const int32_t SpawnLootIndex = SpawnLootCall ? VirtualSlot(SpawnLootCall) : -1;
    // IsAlreadySearched: that first virtual (3.0 slot 0x1A1, 3.3 0x1A3), three slots before SpawnLoot up to 3.3. 3.5 adds
    // a virtual after it (0x1A5, which sets bAlreadySearched), so it is four before (3.5.1 0x1A4, SpawnLoot 0x1A8).
    const int32_t FirstInteractIndex = FirstInteractCall && FirstInteractCall != SpawnLootCall ? VirtualSlot(FirstInteractCall) : -1;
    const int32_t IsAlreadySearchedIndex = FirstInteractIndex >= 0 && FirstInteractIndex < SpawnLootIndex ? FirstInteractIndex : (SpawnLootIndex >= 0 ? SpawnLootIndex - 3 : -1);

    // Three slots after SpawnLoot the container hides itself and records it as searched: lea r8d, [r9+4], then a tail jmp
    // (or call) to AFortWorldManager::UpdateActorRecord. 3.2 adds two virtuals after OnPickupSpawn (0xA2A550 twice), so
    // HideContainer moves to five slots after SpawnLoot (3.2 0xA28E20; 3.1 0xA3F6D0 is at three).
    void** ContainerVTable = ContainerDefault ? *reinterpret_cast<void***>(ContainerDefault) : nullptr;
    int32_t HideContainerIndex = -1;
    uintptr_t HideContainer = 0;
    uintptr_t HideContainerEnd = 0;
    uintptr_t SearchedState = 0;
    for (int32_t Index = SpawnLootIndex >= 0 ? SpawnLootIndex + 3 : 0; SpawnLootIndex >= 0 && !SearchedState && Index <= SpawnLootIndex + 5; Index += 2)
    {
        HideContainerIndex = Index;
        HideContainer = VTableEntry(ContainerVTable, Index);
        HideContainerEnd = HideContainer ? Finder::FunctionEnd(HideContainer) : 0;
        SearchedState = HideContainerEnd ? FindInstruction(HideContainer, HideContainerEnd, { 0x45, 0x8D, 0x41, 0x04 }) : 0;
    }
    // 3.2 moves the container's tier and mesh roll out of PostUpdate into ABuildingContainer::InitializeTierGroupData
    // (21.40's name), the first of the two new virtuals. The container's PostUpdate (3.2 0xA364A0) calls the stripped
    // ABuildingSMActor::PostUpdate, keeps the random list's next value in SavedReservedRandomValueResult, then tail-jumps
    // to InitializeTierGroupData(State, false) (jmp [rax+0xD38], slot 0x1A7), which is stripped to a GetWorld tail. Up to
    // 3.1 the override only calls the stripped parent, then GetWorld. 4.1 (0xAD85D0) calls InitializeTierGroupData
    // (call [rax+0xD58], slot 0x1AB) and goes on to GetWorld (slot 0x28) for its Athena check, so the slot is the first
    // virtual call's, not the last's (#288).
    const uintptr_t ContainerPostUpdate = PostUpdateIndex >= 0 ? VTableEntry(ContainerVTable, PostUpdateIndex) : 0;
    const uintptr_t ContainerPostUpdateEnd = ContainerPostUpdate ? Finder::FunctionEnd(ContainerPostUpdate) : 0;
    const uintptr_t InitializeTierGroupDataCall = ContainerPostUpdateEnd ? FindVirtualCall(ContainerPostUpdate, ContainerPostUpdateEnd) : 0;
    int32_t InitializeTierGroupDataIndex = InitializeTierGroupDataCall ? VirtualSlot(InitializeTierGroupDataCall) : -1;
    if (SpawnLootIndex < 0 || HideContainerIndex != SpawnLootIndex + 5 || InitializeTierGroupDataIndex != SpawnLootIndex + 3)
    {
        InitializeTierGroupDataIndex = -1;
    }

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
    // 2.5 drops the log. The override is still there (0xA2BB80, slot 0xF as on 2.4.2): after Super::PostLoad it points
    // SearchSpeed's curve table at the game data's (mov [rbx+SearchSpeed], rax; 2.4.2 0x4AA41E does the same), which
    // no other UObject-level override of ABuildingContainer writes.
    if (!PostLoad && ContainerVTable && ContainerClass)
    {
        const UProperty* SearchSpeedProperty = ContainerClass->FindPropertyByName(FName(L"SearchSpeed", FNAME_Find));
        const int32_t SearchSpeed = SearchSpeedProperty ? SearchSpeedProperty->GetOffset_ForInternal() : -1;
        int32_t Stores = 0;
        for (int32_t Index = 0; SearchSpeed > 0 && Index < 0x40; ++Index)
        {
            const uintptr_t Function = VTableEntry(ContainerVTable, Index);
            const uintptr_t FunctionEnd = IsInText(Function) ? Finder::FunctionEnd(Function) : 0;
            for (uintptr_t Store = Function; FunctionEnd && Store + 7 <= FunctionEnd && Store < Function + 0x100; ++Store)
            {
                if ((Byte(Store) & 0xF8) == 0x48 && Byte(Store + 1) == 0x89 && (Byte(Store + 2) & 0xC0) == 0x80 && (Byte(Store + 2) & 0x07) != 0x04 && *reinterpret_cast<const int32_t*>(Store + 3) == SearchSpeed)
                {
                    PostLoad = Function;
                    ++Stores;
                    break;
                }
            }
        }
        PostLoad = Stores == 1 ? PostLoad : 0;
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
        Log::Error(L"Finder: building loot not found (SelectMeshSet {}, SetMeshSet {}, PostUpdate {}, SpawnLoot {}, UpdateActorRecord {}, PostLoad {}, ModifyStat {}; hide 0x{:X}, searched 0x{:X})",
            SelectMeshSet != 0, SetMeshSetIndex, PostUpdateIndex, SpawnLootIndex, UpdateActorRecord != 0, PostLoadIndex, ModifyStatIndex,
            HideContainer ? HideContainer - Finder::ImageBase() : 0, SearchedState ? SearchedState - Finder::ImageBase() : 0);
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
    Natives::ABuildingContainer_HideContainerIndex = HideContainerIndex;
    Natives::ABuildingContainer_InitializeTierGroupDataIndex = InitializeTierGroupDataIndex;
    Natives::ABuildingContainer_IsAlreadySearchedIndex = IsAlreadySearchedIndex;
    Natives::ABuildingContainer_SpawnLootIndex = SpawnLootIndex;
    Log::Info(L"Finder: building loot: PostUpdate slot 0x{:X} ({}), SetMeshSet slot 0x{:X}, IsAlreadySearched slot 0x{:X}, SpawnLoot slot 0x{:X}, InitializeTierGroupData slot {}, HideContainer slot 0x{:X}, PostLoad slot 0x{:X} ({}), ModifyStat slot 0x{:X}, UpdateActorRecord 0x{:X}",
        PostUpdateIndex, bPostUpdateSelectsMeshSet ? L"native" : L"stripped", SetMeshSetIndex, IsAlreadySearchedIndex, SpawnLootIndex, InitializeTierGroupDataIndex >= 0 ? std::format(L"0x{:X}", InitializeTierGroupDataIndex) : std::wstring(L"none"), HideContainerIndex, PostLoadIndex, bPostLoadSelectsMeshSet ? L"native" : L"stripped", ModifyStatIndex, UpdateActorRecord - Finder::ImageBase());
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

    // ABuildingSMActor::DetermineMaxResourcesToSpawn survives in 1.2. Its name is the context string it hands FindRow: a
    // function-local static FString up to 6.10, built on the first call inside the function (6.10 0xCF7C94 in 0xCF7A30).
    // 6.20 builds that FString at startup instead (0xDB610: mov rcx, [FString's data]; lea rdx, name), and the function
    // (slot 0x13B, 0xD4C0E0) passes the FString itself: lea rdx, [FString] (0x4F66E40).
    const wchar_t* const DetermineMaxResourcesToSpawnName = L"ABuildingSMActor::DetermineMaxResourcesToSpawn";
    const uintptr_t DetermineMaxResourcesToSpawn = FunctionOf(DetermineMaxResourcesToSpawnName);
    int32_t DetermineMaxResourcesToSpawnIndex = FirstVTableIndex(SMActorVTable, DetermineMaxResourcesToSpawn);
    for (const uintptr_t Ref : DetermineMaxResourcesToSpawnIndex < 0 ? Finder::StringRefs(DetermineMaxResourcesToSpawnName) : std::vector<uintptr_t>{})
    {
        const uintptr_t Load = Ref - 7;
        if (Byte(Load) != 0x48 || Byte(Load + 1) != 0x8B || (Byte(Load + 2) & 0xC7) != 0x05)
        {
            continue;
        }
        const uintptr_t StaticName = Finder::RelativeTarget(Load, 3, 7);
        for (const uintptr_t Site : Finder::AddressRefs(StaticName))
        {
            DetermineMaxResourcesToSpawnIndex = FirstVTableIndex(SMActorVTable, Finder::FunctionStart(Site));
            if (DetermineMaxResourcesToSpawnIndex >= 0)
            {
                break;
            }
        }
        if (DetermineMaxResourcesToSpawnIndex >= 0)
        {
            break;
        }
    }

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

bool Finder::ResolveClearAnimatingMIDsLive(ptrdiff_t ClearMIDOffset, uint8_t ClearMIDMask, ptrdiff_t AnimatingMIDsOffset)
{
    if (Natives::ABuildingSMActor_ClearAnimatingMIDsIndex >= 0)
    {
        return true;
    }

    // ABuildingSMActor's damage visuals update (a virtual: 1.7.2 slot 0x140, 0x4894B0; 1.2 0x1404200D0) clears the animating MIDs
    // when the building is back to full health and bClearMIDWhenReturningToUndamagedState is set:
    // test dword ptr [rdi+780h], 8000h; jz; mov rax, [rdi]; mov rcx, rdi; call [rax+9F8h] (1.2: 770h, 9E8h).
    const uint32_t ByteOffset = static_cast<uint32_t>(ClearMIDOffset);
    const uint32_t DwordOffset = ByteOffset & ~3u;
    const uint32_t DwordMask = static_cast<uint32_t>(ClearMIDMask) << (8 * (ByteOffset & 3));
    const std::initializer_list<int32_t> DwordTest = { 0xF7, -1, static_cast<int32_t>(DwordOffset & 0xFF), static_cast<int32_t>((DwordOffset >> 8) & 0xFF), static_cast<int32_t>((DwordOffset >> 16) & 0xFF), static_cast<int32_t>(DwordOffset >> 24), static_cast<int32_t>(DwordMask & 0xFF), static_cast<int32_t>((DwordMask >> 8) & 0xFF), static_cast<int32_t>((DwordMask >> 16) & 0xFF), static_cast<int32_t>(DwordMask >> 24) };
    const std::initializer_list<int32_t> ByteTest = { 0xF6, -1, static_cast<int32_t>(ByteOffset & 0xFF), static_cast<int32_t>((ByteOffset >> 8) & 0xFF), static_cast<int32_t>((ByteOffset >> 16) & 0xFF), static_cast<int32_t>(ByteOffset >> 24), ClearMIDMask };
    void** SMActorVTable = CdoVTable(L"/Script/FortniteGame.BuildingSMActor");
    std::vector<uintptr_t> Tests;
    for (int32_t Index = 0; SMActorVTable && ClearMIDOffset > 0 && Index < 0x400; ++Index)
    {
        const uintptr_t Entry = VTableEntry(SMActorVTable, Index);
        const uintptr_t EntryEnd = Entry && Finder::FunctionStart(Entry) == Entry ? Finder::FunctionEnd(Entry) : 0;
        if (!Entry)
        {
            break;
        }
        for (const std::initializer_list<int32_t>* Pattern : { &DwordTest, &ByteTest })
        {
            for (uintptr_t Test = EntryEnd ? FindInstruction(Entry, EntryEnd, *Pattern) : 0; Test; Test = FindInstruction(Test + 1, EntryEnd, *Pattern))
            {
                const uint8_t ModRm = Byte(Test + 1);
                if ((ModRm & 0xC0) == 0x80 && (ModRm & 0x38) == 0 && (ModRm & 0x07) != 0x04 && std::find(Tests.begin(), Tests.end(), Test) == Tests.end())
                {
                    Tests.push_back(Test);
                }
            }
        }
    }

    // The cleared MIDs: ABuildingSMActor::ClearAnimatingMIDs opens with cmp dword ptr [rcx+AnimatingMIDs.Num], 0.
    const uint32_t NumOffset = static_cast<uint32_t>(AnimatingMIDsOffset + 8);
    int32_t ClearIndex = -1;
    int32_t Matches = 0;
    for (const uintptr_t Test : Tests)
    {
        const int32_t Index = VirtualSlot(FindVirtualCall(Test, Test + 0x20));
        const uintptr_t Clear = VTableEntry(SMActorVTable, Index);
        if (Clear && Index != ClearIndex && AnimatingMIDsOffset > 0 && FindInstruction(Clear, Clear + 0x40, { 0x83, -1, static_cast<int32_t>(NumOffset & 0xFF), static_cast<int32_t>((NumOffset >> 8) & 0xFF), static_cast<int32_t>((NumOffset >> 16) & 0xFF), static_cast<int32_t>(NumOffset >> 24), 0x00 }))
        {
            ClearIndex = Index;
            ++Matches;
        }
    }

    if (Matches != 1)
    {
        Log::Warning(L"Finder: ABuildingSMActor::ClearAnimatingMIDs not found ({} bitfield tests, {} matches)", Tests.size(), Matches);
        return false;
    }
    Natives::ABuildingSMActor_ClearAnimatingMIDsIndex = ClearIndex;
    Natives::ABuildingSMActor_ClearAnimatingMIDs = VTableEntry(SMActorVTable, ClearIndex);
    Log::Info(L"Finder: ABuildingSMActor::ClearAnimatingMIDs is slot 0x{:X} (+0x{:X})", ClearIndex, Natives::ABuildingSMActor_ClearAnimatingMIDs - Finder::ImageBase());
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

bool Finder::ResolveVoiceConfig()
{
    if (Natives::FConfigCacheIni_GetBool)
    {
        return true;
    }

    // Every reader of [OnlineSubsystem] bHasVoiceEnabled (the voice interface's Init, the subsystems' voice checks; 1.7.2
    // 0x391CCC0, 0x3B42F30, 0x3B43F80) loads the key and calls GConfig->GetBool(Section, Key, Value, GEngineIni) next.
    std::unordered_map<uintptr_t, int32_t> Targets;
    for (const uintptr_t Ref : Finder::StringRefs(L"bHasVoiceEnabled"))
    {
        const uintptr_t Target = Finder::NextCall(Ref, 0x40);
        if (IsInText(Target))
        {
            ++Targets[Target];
        }
    }
    const auto Best = std::max_element(Targets.begin(), Targets.end(), [](const auto& A, const auto& B) { return A.second < B.second; });
    if (Best == Targets.end() || Best->second < 2)
    {
        Log::Error(L"Finder: FConfigCacheIni::GetBool not found from the voice switch's readers");
        return false;
    }

    Natives::FConfigCacheIni_GetBool = reinterpret_cast<decltype(Natives::FConfigCacheIni_GetBool)>(Best->first);
    Log::Info(L"Finder: FConfigCacheIni::GetBool at 0x{:X} (called by {} voice switch readers)", Best->first - Finder::ImageBase(), Best->second);
    return true;
}

bool Finder::ResolvePushToTalk()
{
    if (Natives::AFortPlayerController_PushToTalkPressedIndex >= 0)
    {
        return true;
    }

    // AFortPlayerController's input setup (1.7.2 0x44B150) binds the PushToTalk action, pressed then released, to two
    // virtuals through vcall thunks: lea rax, Thunk shortly before each lea rdx, "PushToTalk", and the thunk is
    // mov rax, [rcx]; jmp [rax+disp32]. 1.7.2: slots 0x324 and 0x325 (0x438810, 0x4389F0), which only reach Vivox.
    std::vector<int32_t> Slots;
    for (const uintptr_t Ref : Finder::StringRefs("PushToTalk"))
    {
        for (uintptr_t At = Ref - 7; At + 0x20 > Ref; --At)
        {
            if (Byte(At) != 0x48 || Byte(At + 1) != 0x8D || Byte(At + 2) != 0x05)
            {
                continue;
            }
            const uintptr_t Thunk = Finder::RelativeTarget(At, 3, 7);
            if (IsInText(Thunk) && FindInstruction(Thunk, Thunk + 5, { 0x48, 0x8B, 0x01, 0xFF, 0xA0 }) == Thunk)
            {
                Slots.push_back(*reinterpret_cast<const int32_t*>(Thunk + 5) / static_cast<int32_t>(sizeof(void*)));
                break;
            }
        }
    }
    if (Slots.size() != 2 || Slots[0] == Slots[1])
    {
        Log::Warning(L"Finder: no PushToTalk bindings found ({} vcall thunks); push to talk stays with Vivox", Slots.size());
        return false;
    }

    Natives::AFortPlayerController_PushToTalkPressedIndex = Slots[0];
    Natives::AFortPlayerController_PushToTalkReleasedIndex = Slots[1];
    Log::Info(L"Finder: AFortPlayerController push to talk is slot 0x{:X} pressed, 0x{:X} released", Slots[0], Slots[1]);
    return true;
}

bool Finder::ResolveConfigCombine()
{
    if (Natives::FConfigFile_CombineFromBuffer)
    {
        return true;
    }
    if (!Finder::ResolveVoiceConfig() || !Finder::ResolveOnlineVoice())
    {
        Log::Error(L"Finder: config merge prerequisites missing (GetBool or FOnlineVoiceImpl)");
        return false;
    }

    // FConfigCacheIni::GetBool (1.9.1 0x131A1A0) starts by calling GetString (0x131D1A0), whose first call with a false
    // third argument (xor r8d, r8d) is FConfigCacheIni::Find(Filename, false) (0x1316B30).
    uintptr_t GetString = 0;
    Finder::TraceFind(reinterpret_cast<uintptr_t>(Natives::FConfigCacheIni_GetBool), 16, [&](uintptr_t Instruction)
    {
        GetString = Byte(Instruction) == 0xE8 ? Finder::RelativeTarget(Instruction, 1, 5) : 0;
        return GetString != 0;
    });
    bool bNoCreate = false;
    uintptr_t Find = 0;
    Finder::TraceFind(GetString, 48, [&](uintptr_t Instruction)
    {
        if (Byte(Instruction) == 0x45 && Byte(Instruction + 1) == 0x33 && Byte(Instruction + 2) == 0xC0)
        {
            bNoCreate = true;
            return false;
        }
        Find = bNoCreate && Byte(Instruction) == 0xE8 ? Finder::RelativeTarget(Instruction, 1, 5) : 0;
        return Find != 0;
    });

    // FOnlineVoiceImpl::Init (1.9.1 0x3A5BAE0) opens with GConfig->GetInt(L"OnlineSubsystem", L"MaxLocalTalkers", ...,
    // GEngineIni): mov rcx, [GConfig]; lea rsi, [GEngineIni]; ...; mov [rsp+20h], rsi; call.
    uintptr_t Config = 0;
    uintptr_t EngineIni = 0;
    uintptr_t Loaded[16] = {};
    // The build's own Init: read before FOnlineVoiceImpl::Hook swaps the slot for Core's.
    const uintptr_t VoiceInit = reinterpret_cast<uintptr_t>(Natives::FOnlineVoiceImpl_VTable[Natives::FOnlineVoiceImpl_InitIndex]);
    const bool bVoiceInitInExe = std::any_of(Finder::ExecutableSections().begin(), Finder::ExecutableSections().end(), [&](const auto& Section) { return VoiceInit >= Section.first && VoiceInit < Section.second; });
    Finder::TraceFind(bVoiceInitInExe ? VoiceInit : 0, 32, [&](uintptr_t Instruction)
    {
        const uint8_t Rex = Byte(Instruction);
        if ((Rex == 0x48 || Rex == 0x4C) && Byte(Instruction + 1) == 0x8B && Byte(Instruction + 2) == 0x0D && !Config)
        {
            Config = Finder::RelativeTarget(Instruction, 3, 7);
        }
        else if ((Rex == 0x48 || Rex == 0x4C) && Byte(Instruction + 1) == 0x8D && (Byte(Instruction + 2) & 0xC7) == 0x05)
        {
            Loaded[((Byte(Instruction + 2) >> 3) & 7) + (Rex == 0x4C ? 8 : 0)] = Finder::RelativeTarget(Instruction, 3, 7);
        }
        else if ((Rex == 0x48 || Rex == 0x4C) && Byte(Instruction + 1) == 0x89 && (Byte(Instruction + 2) & 0xC7) == 0x44 && Byte(Instruction + 3) == 0x24 && Byte(Instruction + 4) == 0x20)
        {
            EngineIni = Loaded[((Byte(Instruction + 2) >> 3) & 7) + (Rex == 0x4C ? 8 : 0)];
        }
        return Byte(Instruction) == 0xE8;
    });

    // UOnlineHotfixManager::HotfixIniFile (1.9.1 0x3C68890, which logs how long the update took): GetConfigFile,
    // then ConfigFile->CombineFromBuffer(IniData) (0x1310D80) as its third call. CombineFromBuffer reads the buffer a
    // line at a time, skipping CR and LF first. 4.2 asks FileName.Find(TEXT("Engine.ini")) first (0x626650), which makes
    // CombineFromBuffer (0x130B040) the fourth call there.
    const auto ReadsLines = [](uintptr_t Function)
    {
        return Function
            && Finder::TraceFind(Function, 48, [](uintptr_t Instruction) { return Byte(Instruction) == 0x66 && Byte(Instruction + 1) == 0x83 && Byte(Instruction + 3) == 0x0D; })
            && Finder::TraceFind(Function, 48, [](uintptr_t Instruction) { return Byte(Instruction) == 0x66 && Byte(Instruction + 1) == 0x83 && Byte(Instruction + 3) == 0x0A; });
    };
    uintptr_t Combine = 0;
    int32_t Calls = 0;
    Finder::TraceFind(FunctionOf(L"Updating config from %s took %f seconds and reloaded %d objects"), 64, [&](uintptr_t Instruction)
    {
        if (Byte(Instruction) != 0xE8 || ++Calls < 3)
        {
            return false;
        }
        Combine = Finder::RelativeTarget(Instruction, 1, 5);
        return ReadsLines(Combine) || Calls >= 4;
    });
    const bool bCombineReadsLines = ReadsLines(Combine);

    if (!Find || !Config || !EngineIni || !bCombineReadsLines)
    {
        Log::Error(L"Finder: config merge not found (Find 0x{:X}, GConfig 0x{:X}, GEngineIni 0x{:X}, CombineFromBuffer 0x{:X})",
            Find ? Find - Finder::ImageBase() : 0, Config ? Config - Finder::ImageBase() : 0, EngineIni ? EngineIni - Finder::ImageBase() : 0, Combine ? Combine - Finder::ImageBase() : 0);
        return false;
    }

    Natives::FConfigCacheIni_Find = reinterpret_cast<decltype(Natives::FConfigCacheIni_Find)>(Find);
    Natives::GConfig = reinterpret_cast<void**>(Config);
    Natives::GEngineIni = reinterpret_cast<const FString*>(EngineIni);
    Natives::FConfigFile_CombineFromBuffer = reinterpret_cast<decltype(Natives::FConfigFile_CombineFromBuffer)>(Combine);
    Log::Info(L"Finder: config merge: FConfigCacheIni::Find +0x{:X}, GConfig +0x{:X}, GEngineIni +0x{:X}, FConfigFile::CombineFromBuffer +0x{:X}",
        Find - Finder::ImageBase(), Config - Finder::ImageBase(), EngineIni - Finder::ImageBase(), Combine - Finder::ImageBase());
    return true;
}

bool Finder::ResolveEngineInit()
{
    if (Natives::UEngine_InitIndex >= 0)
    {
        return true;
    }

    // FEngineLoop::Init (1.9.1 0x27C4EB): GEngine = NewObject<UEngine>(...), GEngine->ParseCommandline(), the log
    // "Initializing Game Engine...", then GEngine->Init(this): mov rcx, [GEngine]; mov rdx, ...; mov rax, [rcx];
    // call [rax+X] (1.9.1 +0x248). The first virtual call on the object loaded from GEngine after the log is Init.
    const uintptr_t Engine = reinterpret_cast<uintptr_t>(Natives::GEngine);
    for (const uintptr_t Ref : Finder::StringRefs(L"Initializing Game Engine..."))
    {
        bool bLoaded = false;
        const uintptr_t Call = Finder::TraceFind(Ref, 32, [&](uintptr_t Instruction)
        {
            if (Byte(Instruction) == 0x48 && Byte(Instruction + 1) == 0x8B && Byte(Instruction + 2) == 0x0D && Finder::RelativeTarget(Instruction, 3, 7) == Engine)
            {
                bLoaded = true;
                return false;
            }
            return bLoaded && Byte(Instruction) == 0xFF && (Byte(Instruction + 1) == 0x90 || Byte(Instruction + 1) == 0x50);
        });
        if (!Call)
        {
            continue;
        }

        const int32_t Displacement = Byte(Call + 1) == 0x90 ? *reinterpret_cast<const int32_t*>(Call + 2) : static_cast<int8_t>(Byte(Call + 2));
        if (Displacement <= 0 || Displacement % sizeof(void*) != 0)
        {
            continue;
        }
        Natives::UEngine_InitIndex = Displacement / static_cast<int32_t>(sizeof(void*));
        Log::Info(L"Finder: UEngine::Init is slot 0x{:X}", Natives::UEngine_InitIndex);
        return true;
    }

    Log::Error(L"Finder: UEngine::Init's call in FEngineLoop::Init not found");
    return false;
}

bool Finder::ResolveMcpBasicAuth()
{
    if (Natives::FBaseServiceMcp_GetBasicAuth)
    {
        return true;
    }

    // FBaseServiceMcp::GetBasicAuth(ClientId) (1.9.1 0x3AB6F50, BaseServiceMcp.cpp) looks the client up in its service's
    // ServicePermissions and logs this when it isn't there. It's virtual and no service class overrides it, so the
    // vtable of every service's FBaseServiceMcp part holds it.
    const uintptr_t BasicAuth = FunctionOf(L"Missing client permissions. id=%s");
    if (!BasicAuth || Finder::ReadOnlySlotsHolding(BasicAuth).empty())
    {
        Log::Error(L"Finder: FBaseServiceMcp::GetBasicAuth not found");
        return false;
    }
    Natives::FBaseServiceMcp_GetBasicAuth = reinterpret_cast<void*>(BasicAuth);
    Log::Info(L"Finder: FBaseServiceMcp::GetBasicAuth +0x{:X}", BasicAuth - Finder::ImageBase());
    return true;
}

bool Finder::ResolveOnlineVoice()
{
    if (Natives::FOnlineVoiceImpl_VTable)
    {
        return true;
    }

    // FOnlineVoiceImpl (OnlineSubsystemUtils; 1.7.2 vtable 0x4C92CE0): Init at slot 0 (0x391CCC0, which logs a missing
    // MaxLocalTalkers key; so does the voice Exec's dump), RegisterRemoteTalker a few slots on (0x39294E0), and
    // SerializeRemotePacket right after UnmuteRemoteTalker (0x3931590, 0x392C690).
    const uintptr_t Register = FunctionOf(L"RegisterRemoteTalker(%s) returned 0x%08X");
    const uintptr_t Unmute = FunctionOf(L"Unmuting remote talker (%s)");
    std::vector<uintptr_t> Inits;
    for (const uintptr_t Ref : Finder::StringRefs(L"Missing MaxLocalTalkers key in OnlineSubsystem of DefaultEngine.ini"))
    {
        Inits.push_back(Finder::FunctionStart(Ref));
    }

    void** const RegisterEntry = Finder::VTableContaining(Register, 0, 1);
    void** VTable = nullptr;
    for (int32_t Back = 0; RegisterEntry && Back < 16 && !VTable; ++Back)
    {
        if (std::find(Inits.begin(), Inits.end(), reinterpret_cast<uintptr_t>(RegisterEntry[-Back])) != Inits.end())
        {
            VTable = RegisterEntry - Back;
        }
    }
    int32_t UnmuteIndex = -1;
    for (int32_t Index = 0; VTable && Index < 64 && UnmuteIndex < 0; ++Index)
    {
        UnmuteIndex = reinterpret_cast<uintptr_t>(VTable[Index]) == Unmute ? Index : -1;
    }
    if (!VTable || UnmuteIndex < 0 || !VTableEntry(VTable, UnmuteIndex + 1))
    {
        Log::Warning(L"Finder: FOnlineVoiceImpl vtable not found (RegisterRemoteTalker 0x{:X}, {} Init candidates)", Register ? Register - Finder::ImageBase() : 0, Inits.size());
        return false;
    }

    // RegisterLocalTalker (1.7.2 0x3928930, slot 5) logs its result.
    const uintptr_t RegisterLocal = FunctionOf(L"RegisterLocalTalker(%d) returned 0x%08X");
    int32_t RegisterLocalIndex = -1;
    for (int32_t Index = 0; RegisterLocal && Index < 64 && RegisterLocalIndex < 0; ++Index)
    {
        RegisterLocalIndex = reinterpret_cast<uintptr_t>(VTable[Index]) == RegisterLocal ? Index : -1;
    }

    Natives::FOnlineVoiceImpl_VTable = VTable;
    Natives::FOnlineVoiceImpl_InitIndex = 0;
    Natives::FOnlineVoiceImpl_SerializeRemotePacketIndex = UnmuteIndex + 1;
    Natives::FOnlineVoiceImpl_RegisterLocalTalkerIndex = RegisterLocalIndex;
    // GetLocalPacket follows SerializeRemotePacket, as in IOnlineVoice (1.7.2 slot 0x16, 0x3919750).
    Natives::FOnlineVoiceImpl_GetLocalPacketIndex = VTableEntry(VTable, UnmuteIndex + 2) ? UnmuteIndex + 2 : -1;

    // SerializeRemotePacket (1.7.2 0x392C690) asks the new packet for its sender, its only virtual call with an out
    // parameter (FVoicePacket::GetSender, slot 3: lea rdx, [rsp+x]; mov rax, [rcx]; call [rax+18h]), once the subsystem's
    // IsDedicated (mov rax, [rcx]; call [rax+0E8h]) says it's a client. Tick (slot 0x19, 0x392EA40), a few slots after
    // GetLocalPacket, makes that IsDedicated call first too.
    const uintptr_t Serialize = VTableEntry(VTable, UnmuteIndex + 1);
    const uintptr_t SerializeEnd = Serialize ? Finder::FunctionEnd(Serialize) : 0;
    const uintptr_t GetSenderCall = SerializeEnd > Serialize ? FindInstruction(Serialize, SerializeEnd, { 0x48, 0x8D, 0x54, 0x24, -1, 0x48, 0x8B, 0x01, 0xFF, 0x50, -1 }) : 0;
    int32_t GetSenderDisplacement = GetSenderCall ? Byte(GetSenderCall + 10) : -1;
    // 5.0 (UE 4.21) schedules a stack store between the out parameter and the call: lea rdx, [rsp+30h]; mov [rsp+60h],
    // rbp; mov rax, [rcx]; call [rax+18h] (CL-4214610 0x41813D).
    for (uintptr_t Call = !GetSenderCall && SerializeEnd > Serialize ? FindInstruction(Serialize, SerializeEnd, { 0x48, 0x8B, 0x01, 0xFF, 0x50, -1 }) : 0; Call && GetSenderDisplacement < 0; Call = FindInstruction(Call + 1, SerializeEnd, { 0x48, 0x8B, 0x01, 0xFF, 0x50, -1 }))
    {
        if (Call >= Serialize + 0x10 && FindInstruction(Call - 0x10, Call, { 0x48, 0x8D, 0x54, 0x24, -1 }))
        {
            GetSenderDisplacement = Byte(Call + 5);
        }
    }
    const uintptr_t IsDedicatedCall = SerializeEnd > Serialize ? FindInstruction(Serialize, SerializeEnd, { 0x48, 0x8B, 0x01, 0xFF, 0x90 }) : 0;
    const int32_t IsDedicatedOffset = IsDedicatedCall ? *reinterpret_cast<const int32_t*>(IsDedicatedCall + 5) : 0;
    int32_t TickIndex = -1;
    for (int32_t Index = UnmuteIndex + 3; IsDedicatedOffset > 0 && Index <= UnmuteIndex + 6 && TickIndex < 0; ++Index)
    {
        const uintptr_t Entry = VTableEntry(VTable, Index);
        const uintptr_t EntryEnd = Entry ? (std::min)(Finder::FunctionEnd(Entry), Entry + 0x30) : 0;
        const uintptr_t Call = EntryEnd > Entry ? FindInstruction(Entry, EntryEnd, { 0x48, 0x8B, 0x01, 0xFF, 0x90 }) : 0;
        TickIndex = Call && *reinterpret_cast<const int32_t*>(Call + 5) == IsDedicatedOffset ? Index : -1;
    }
    Natives::FVoicePacket_GetSenderIndex = GetSenderDisplacement >= 0 && GetSenderDisplacement % 8 == 0 ? GetSenderDisplacement / 8 : -1;
    Natives::FOnlineVoiceImpl_TickIndex = TickIndex;
    if (Natives::FVoicePacket_GetSenderIndex < 0 || TickIndex < 0)
    {
        Log::Warning(L"Finder: FVoicePacket::GetSender slot {} or FOnlineVoiceImpl::Tick slot {} not found; the HUD won't show who's talking", Natives::FVoicePacket_GetSenderIndex, TickIndex);
    }
    Log::Info(L"Finder: FOnlineVoiceImpl vtable at 0x{:X}, SerializeRemotePacket slot 0x{:X}, RegisterLocalTalker slot 0x{:X}, Tick slot 0x{:X}, packet GetSender slot {}", reinterpret_cast<uintptr_t>(VTable) - Finder::ImageBase(), UnmuteIndex + 1, RegisterLocalIndex, TickIndex, Natives::FVoicePacket_GetSenderIndex);
    return true;
}

bool Finder::ResolveVoiceHandshake()
{
    if (Natives::APlayerController_VoiceHandshakeCompletedOffset >= 0)
    {
        return true;
    }

    // ClientVoiceHandshakeComplete's exec tail-jumps to its _Implementation (mov rax, [rcx]; jmp [rax+disp32]; 1.7.2
    // 0x25D15D0, slot 0x130), which is mov byte ptr [rcx+MuteList.bHasVoiceHandshakeCompleted], 1 (0x2315920: +0x570).
    const UFunction* Function = FindObject<UFunction>(nullptr, L"/Script/Engine.PlayerController:ClientVoiceHandshakeComplete");
    const uintptr_t Exec = Function ? reinterpret_cast<uintptr_t>(Function->GetNativeFunc()) : 0;
    // The jump may carry a REX prefix (48 FF A0).
    const uintptr_t Load = IsInText(Exec) ? FindInstruction(Exec, Exec + 0x40, { 0x48, 0x8B, 0x01 }) : 0;
    const uintptr_t Jump = Load ? Load + 3 + (Byte(Load + 3) == 0x48 ? 1 : 0) : 0;
    const int32_t Slot = Jump && Byte(Jump) == 0xFF && Byte(Jump + 1) == 0xA0 ? *reinterpret_cast<const int32_t*>(Jump + 2) / static_cast<int32_t>(sizeof(void*)) : -1;
    const uintptr_t Implementation = Slot >= 0 ? VTableEntry(CdoVTable(L"/Script/Engine.PlayerController"), Slot) : 0;
    const bool bSetsFlag = Implementation && Byte(Implementation) == 0xC6 && Byte(Implementation + 1) == 0x81 && Byte(Implementation + 6) == 0x01;
    if (!bSetsFlag)
    {
        Log::Warning(L"Finder: APlayerController's voice handshake flag not found (ClientVoiceHandshakeComplete slot {})", Slot);
        return false;
    }

    Natives::APlayerController_VoiceHandshakeCompletedOffset = *reinterpret_cast<const int32_t*>(Implementation + 2);
    Log::Info(L"Finder: APlayerController MuteList.bHasVoiceHandshakeCompleted is +0x{:X}", Natives::APlayerController_VoiceHandshakeCompletedOffset);
    return true;
}

bool Finder::ResolveBuildingRestorationLive(ptrdiff_t EditingPlayerOffset)
{
    static bool bSearched = false;
    if (Natives::ABuildingSMActor_RepairBuildingIndex >= 0 || bSearched)
    {
        return Natives::ABuildingSMActor_RepairBuildingIndex >= 0;
    }
    bSearched = true;

    // 1.8.1 (CL-3729133) strips AFortPlayerController's ServerCreate/Edit/BeginEditing/EndEditing/Repair/Upgrade
    // BuildingActor _Implementations (the shared empty function), but every helper the 1.8 bodies call is still there.
    // Their slots come from three client functions that make the same checks before they send the RPCs. Each is the only
    // caller of its RPC's stub, which loads the RPC's name global (filled by its static FName initializer):
    // - Place (ServerCreateBuildingActor; 1.8.1 0x83B170, 1.8 0x8336F0): CanPerformAction(InteractBuild),
    //   IsBuildingRestricted, CanPlaceBuildableClassInStructuralGrid, CanAffordToPlaceBuildableClass, and after the RPC
    //   GetErrorMessageFromStructuralQuery.
    // - ABuildingSMActor's improvement interaction (ServerRepairBuildingActor; 0x49E0A0, 1.8 0x482600): NeedsRepair,
    //   HasPermissionToEditWorld, IsBeingPlayerEdited, CanAffordToRepair, and CanAffordToPlaceBuildableClass for an upgrade.
    // - Begin editing (ServerBeginEditingBuildingActor; 0x83AFD0, 1.8 0x833550): IsBeingPlayerEdited, HasPermissionToEditWorld.
    // The controller declares its helpers in runs, as 21.40 still does: CanAffordToPlaceBuildableClass, CanAffordToRepair,
    // IsBuildingRestricted, HasRequiredBuildingLevel (1.8.1 0x2A6-0x2A9, 1.8 0x2A5-0x2A8), and
    // CanPlaceBuildableClassInStructuralGrid, PayBuildableClassPlacementCost, PayBuildingRepairCost,
    // GetErrorMessageFromStructuralQuery (0x2B2-0x2B5, 1.8 0x2B1-0x2B4). On 1.8 these rules give exactly the slots its own
    // bodies call. 1.8.2 (CL-3741772) strips the same six bodies. APlayerController gains a slot there and
    // AFortPlayerController loses ServerSetTimeOfDay's two, so the helper runs are at 1.8's slots again (0x2A5-0x2A8, 0x2B1-0x2B4; place 0x83AEB0, improve 0x4A0580, begin edit 0x83AD10).
    const auto RpcStub = [](const wchar_t* Name) -> uintptr_t
    {
        for (const uintptr_t Ref : Finder::StringRefs(Name))
        {
            const uintptr_t Store = FindInstruction(Ref + 7, Ref + 0x10, { 0x48, 0x8D, 0x0D });
            const uintptr_t NameGlobal = Store ? Finder::RelativeTarget(Store, 3, 7) : 0;
            int32_t Matches = 0;
            const uintptr_t Load = NameGlobal && !IsInText(NameGlobal) ? ScanRipTarget({ 0x48, 0x8B, 0x15 }, 3, 7, NameGlobal, Matches) : 0;
            if (Load && Matches == 1)
            {
                return Finder::FunctionStart(Load);
            }
        }
        return 0;
    };
    const auto OnlyCaller = [](uintptr_t Function) -> uintptr_t
    {
        uintptr_t Caller = 0;
        for (const uintptr_t Site : Function ? Finder::CallsTo(Function, false) : std::vector<uintptr_t>())
        {
            const uintptr_t Start = Finder::FunctionStart(Site);
            if (Caller && Start != Caller)
            {
                return 0;
            }
            Caller = Start;
        }
        return Caller;
    };
    const auto Slots = [](uintptr_t Function)
    {
        std::vector<int32_t> Result;
        const uintptr_t End = Function && Finder::FunctionStart(Function) == Function ? Finder::FunctionEnd(Function) : 0;
        for (uintptr_t Site = End ? FindVirtualCall(Function, End) : 0; Site; Site = FindVirtualCall(Site + 1, End))
        {
            if (const int32_t Slot = VirtualSlot(Site); Slot >= 0)
            {
                Result.push_back(Slot);
            }
        }
        return Result;
    };
    const auto Has = [](const std::vector<int32_t>& Set, int32_t Slot)
    {
        return Slot >= 0 && std::find(Set.begin(), Set.end(), Slot) != Set.end();
    };
    const auto ImplementationOf = [](const wchar_t* FunctionPath)
    {
        return ImplementationIndex(FindObject<UFunction>(nullptr, FunctionPath));
    };

    const uintptr_t Place = OnlyCaller(RpcStub(L"ServerCreateBuildingActor"));
    const uintptr_t Improve = OnlyCaller(RpcStub(L"ServerRepairBuildingActor"));
    const uintptr_t BeginEdit = OnlyCaller(RpcStub(L"ServerBeginEditingBuildingActor"));
    const std::vector<int32_t> PlaceSlots = Slots(Place);
    const std::vector<int32_t> ImproveSlots = Slots(Improve);
    const std::vector<int32_t> BeginEditSlots = Slots(BeginEdit);

    // CanAffordToPlaceBuildableClass is the one slot both ask whose next slot (CanAffordToRepair) the improvement asks
    // and whose second next (IsBuildingRestricted) the place function asks.
    int32_t CanAfford = -1;
    int32_t Candidates = 0;
    for (const int32_t Slot : PlaceSlots)
    {
        if (Slot != CanAfford && Has(ImproveSlots, Slot) && Has(ImproveSlots, Slot + 1) && Has(PlaceSlots, Slot + 2))
        {
            CanAfford = Slot;
            ++Candidates;
        }
    }
    // CanPlaceBuildableClassInStructuralGrid is the place function's slot past that run whose third next slot
    // (GetErrorMessageFromStructuralQuery) it also asks.
    int32_t CanPlace = -1;
    int32_t PlaceCandidates = 0;
    // 2.5 (0xE1A9E0) no longer asks it itself: its build preview query (slot 0x2BE, 0xE1B880) does, and the place
    // function then asks GetErrorMessageFromStructuralQuery (0x2C2) about the result. So the slots those ask count too.
    std::vector<int32_t> PlaceAndNestedSlots = PlaceSlots;
    void** ControllerVTable = Candidates == 1 ? CdoVTable(L"/Script/FortniteGame.FortPlayerController") : nullptr;
    for (const int32_t Slot : ControllerVTable ? PlaceSlots : std::vector<int32_t>())
    {
        for (const int32_t Nested : Slot > CanAfford + 3 ? Slots(VTableEntry(ControllerVTable, Slot)) : std::vector<int32_t>())
        {
            PlaceAndNestedSlots.push_back(Nested);
        }
    }
    std::sort(PlaceAndNestedSlots.begin(), PlaceAndNestedSlots.end());
    PlaceAndNestedSlots.erase(std::unique(PlaceAndNestedSlots.begin(), PlaceAndNestedSlots.end()), PlaceAndNestedSlots.end());
    for (const int32_t Slot : Candidates == 1 ? PlaceAndNestedSlots : std::vector<int32_t>())
    {
        if (Slot != CanPlace && Slot > CanAfford + 3 && Has(PlaceSlots, Slot + 3))
        {
            CanPlace = Slot;
            ++PlaceCandidates;
        }
    }

    void** BuildingVTable = CdoVTable(L"/Script/FortniteGame.BuildingSMActor");

    // ABuildingActor::SilentDie (its exec calls the slot) is Die with Gameplay.Effect.InstantDeath.Silent: it fetches the
    // gameplay tag singleton (its one direct call, 1.8.1 0x5B5AA0, 1.8 0x5C94C0) and makes one virtual call (1.8.1
    // 0x47C060 -> slot 0xF3).
    const int32_t SilentDie = ImplementationOf(L"/Script/FortniteGame.BuildingActor:SilentDie");
    const uintptr_t SilentDieEntry = VTableEntry(BuildingVTable, SilentDie);
    const uintptr_t SilentDieEnd = SilentDieEntry && Finder::FunctionStart(SilentDieEntry) == SilentDieEntry ? Finder::FunctionEnd(SilentDieEntry) : 0;
    const std::vector<int32_t> SilentDieSlots = Slots(SilentDieEntry);
    const int32_t Die = SilentDieSlots.size() == 1 && SilentDieSlots.front() != SilentDie ? SilentDieSlots.front() : -1;
    const std::vector<std::pair<uintptr_t, uintptr_t>> SilentDieCalls = SilentDieEnd ? Finder::DirectCalls(SilentDieEntry, SilentDieEnd) : std::vector<std::pair<uintptr_t, uintptr_t>>();
    const uintptr_t TagsGetter = SilentDieCalls.size() == 1 ? SilentDieCalls.front().second : 0;

    // The place function first asks whether the pawn may build: it fetches the same singleton, then
    // lea rdx, [rax+Tag]; call [vtable+CanPerformAction] (1.8.1 +0x2C8, 1.8 +0x2C0), and the tag there is
    // Gameplay.Action.Player.InteractBuild.
    int32_t CanPerformAction = -1;
    const FName InteractBuild(L"Gameplay.Action.Player.InteractBuild", FNAME_Find);
    const uintptr_t PlaceEnd = Place ? Finder::FunctionEnd(Place) : 0;
    for (uintptr_t Site = PlaceEnd && TagsGetter && !InteractBuild.IsNone() ? FindVirtualCall(Place, PlaceEnd) : 0; Site && CanPerformAction < 0; Site = FindVirtualCall(Site + 1, PlaceEnd))
    {
        const uintptr_t Lea = Site - 7;
        if (Lea < Place || Byte(Lea) != 0x48 || Byte(Lea + 1) != 0x8D || Byte(Lea + 2) != 0x90 || !CallWithin(Lea - 0x10, 0x10, TagsGetter))
        {
            continue;
        }
        const uintptr_t Tags = reinterpret_cast<uintptr_t (*)()>(TagsGetter)();
        if (Tags && *reinterpret_cast<const FName*>(Tags + *reinterpret_cast<const int32_t*>(Lea + 3)) == InteractBuild)
        {
            CanPerformAction = VirtualSlot(Site);
        }
    }
    // 4.4 (0xF9A9CA) adds the slot to the vtable first and calls through the register, with the controller moved into
    // rcx after the tag getter: mov rbx, [rdi]; add rbx, 13E8h; call TagsGetter; mov rcx, rdi;
    // lea rdx, [rax+Tag]; call qword ptr [rbx].
    for (uintptr_t Lea = PlaceEnd && TagsGetter && !InteractBuild.IsNone() && CanPerformAction < 0 ? FindInstruction(Place, PlaceEnd, { 0x48, 0x8D, 0x90 }) : 0; Lea && CanPerformAction < 0; Lea = FindInstruction(Lea + 1, PlaceEnd, { 0x48, 0x8D, 0x90 }))
    {
        // call qword ptr [reg] (not rsp/rbp) right after the lea.
        const uint8_t ModRm = Byte(Lea + 8);
        if (Byte(Lea + 7) != 0xFF || (ModRm & 0xF8) != 0x10 || (ModRm & 0x07) == 0x04 || (ModRm & 0x07) == 0x05 || !CallWithin(Lea - 0x10, 0x10, TagsGetter))
        {
            continue;
        }
        // add reg, imm32 (48 81 C0+reg) on the same register shortly before.
        uintptr_t Add = 0;
        for (uintptr_t Candidate = Lea - 0x20 > Place ? Lea - 0x20 : Place; Candidate + 7 <= Lea; ++Candidate)
        {
            if (Byte(Candidate) == 0x48 && Byte(Candidate + 1) == 0x81 && Byte(Candidate + 2) == (0xC0 | (ModRm & 0x07)))
            {
                Add = Candidate;
            }
        }
        const int32_t Displacement = Add ? *reinterpret_cast<const int32_t*>(Add + 3) : 0;
        const uintptr_t Tags = Add && Displacement > 0 && Displacement % 8 == 0 ? reinterpret_cast<uintptr_t (*)()>(TagsGetter)() : 0;
        if (Tags && *reinterpret_cast<const FName*>(Tags + *reinterpret_cast<const int32_t*>(Lea + 3)) == InteractBuild)
        {
            CanPerformAction = Displacement / 8;
        }
    }

    const auto BodyIs = [](uintptr_t Entry, std::initializer_list<int32_t> Pattern)
    {
        return FindInstruction(Entry, Entry + Pattern.size(), Pattern) == Entry;
    };
    const int32_t Offset = static_cast<int32_t>(EditingPlayerOffset);
    const int32_t O0 = Offset & 0xFF;
    const int32_t O1 = (Offset >> 8) & 0xFF;
    const int32_t O2 = (Offset >> 16) & 0xFF;
    const int32_t O3 = (Offset >> 24) & 0xFF;
    // IsBeingPlayerEdited: cmp qword ptr [rcx+EditingPlayer], 0 (0xCD). GetEditingPlayer: mov rax, [rcx+EditingPlayer]; ret
    // (0x14D).
    const int32_t IsBeingEdited = Offset > 0 ? FirstVTableIndexWhere(BuildingVTable, [&](uintptr_t Entry) { return BodyIs(Entry, { 0x48, 0x83, 0xB9, O0, O1, O2, O3, 0x00 }); }) : -1;
    const int32_t GetEditing = Offset > 0 ? FirstVTableIndexWhere(BuildingVTable, [&](uintptr_t Entry) { return BodyIs(Entry, { 0x48, 0x8B, 0x81, O0, O1, O2, O3, 0xC3 }); }) : -1;
    // HasPermissionToEditWorld is the other slot both the improvement and begin editing ask (1.8.1 0x2A0, 1.8 0x29F).
    int32_t HasPermission = -1;
    int32_t PermissionCandidates = 0;
    for (const int32_t Slot : Has(ImproveSlots, IsBeingEdited) ? ImproveSlots : std::vector<int32_t>())
    {
        if (Slot != IsBeingEdited && Slot != HasPermission && Has(BeginEditSlots, Slot))
        {
            HasPermission = Slot;
            ++PermissionCandidates;
        }
    }

    // 21.40's ABuildingSMActor order: ReplaceBuildingActor, SupportsEditing, CanBePlayerEdited, IsBeingEdited,
    // GetEditingPlayer, SetEditingPlayer (1.8 and 1.8.1 0x149-0x14E), and OnServerAttemptBuildingUpgrade right before
    // IsBeingPlayerEdited (0xCC, 0xCD). ReplaceBuildingActor spawns the new piece, SetEditingPlayer stores the player
    // state, and the upgrade replaces the piece.
    // Natives::UWorld_SpawnActor is the FTransform overload (the "no class was specified" one, 1.8.1 0x225B5F0). Game
    // code calls the location and rotation overload (0x225C530), a short wrapper that builds the transform and calls it.
    const uintptr_t SpawnActor = reinterpret_cast<uintptr_t>(Natives::UWorld_SpawnActor);
    const auto IsSpawnActor = [SpawnActor](uintptr_t Target)
    {
        const uintptr_t TargetEnd = Target && Finder::FunctionStart(Target) == Target ? Finder::FunctionEnd(Target) : 0;
        return SpawnActor && (Target == SpawnActor || (TargetEnd > Target && TargetEnd - Target < 0x100 && CallWithin(Target, TargetEnd - Target, SpawnActor)));
    };
    const auto CallsSpawnActor = [&IsSpawnActor](uintptr_t Begin, uintptr_t End)
    {
        for (uintptr_t Call = End > Begin ? FindRawCall(Begin, End) : 0; Call; Call = FindRawCall(Call + 1, End))
        {
            if (IsSpawnActor(Finder::RelativeTarget(Call, 1, 5)))
            {
                return Call;
            }
        }
        return static_cast<uintptr_t>(0);
    };

    const int32_t Replace = GetEditing >= 4 ? GetEditing - 4 : -1;
    const int32_t SetEditing = GetEditing >= 0 ? GetEditing + 1 : -1;
    const int32_t Upgrade = IsBeingEdited >= 1 ? IsBeingEdited - 1 : -1;
    const uintptr_t ReplaceEntry = VTableEntry(BuildingVTable, Replace);
    const uintptr_t ReplaceEnd = ReplaceEntry && Finder::FunctionStart(ReplaceEntry) == ReplaceEntry ? Finder::FunctionEnd(ReplaceEntry) : 0;
    const bool bReplaceSpawns = ReplaceEnd > ReplaceEntry && CallsSpawnActor(ReplaceEntry, ReplaceEnd) != 0;
    const uintptr_t SetEditingEntry = VTableEntry(BuildingVTable, SetEditing);
    const bool bSetEditingStores = SetEditingEntry && FindInstruction(SetEditingEntry, SetEditingEntry + 0x60, { 0x48, 0x89, -1, O0, O1, O2, O3 });
    const bool bUpgradeReplaces = Has(Slots(VTableEntry(BuildingVTable, Upgrade)), Replace);

    // AFortAIController::CreateBuildingActor (1.8.1 0x2C1470, 1.8 0x2C1120) spawns its piece and makes three virtual calls
    // on it: SetMirrored, InitializeBuildingActor, PostInitializeSpawnedBuildingActor (0x123, 0xF7, 0xF8).
    const int32_t SetMirrored = ImplementationOf(L"/Script/FortniteGame.BuildingSMActor:SetMirrored");
    const uintptr_t AICreate = ExecImplementation(L"/Script/FortniteGame.FortAIController:CreateBuildingActor");
    const uintptr_t AICreateEnd = AICreate && Finder::FunctionStart(AICreate) == AICreate ? Finder::FunctionEnd(AICreate) : 0;
    const uintptr_t SpawnCall = AICreateEnd ? CallsSpawnActor(AICreate, AICreateEnd) : 0;
    int32_t AfterSpawn[3] = { -1, -1, -1 };
    uintptr_t Site = SpawnCall ? FindVirtualCall(SpawnCall + 5, AICreateEnd) : 0;
    for (int32_t Index = 0; Index < 3 && Site; ++Index, Site = FindVirtualCall(Site + 1, AICreateEnd))
    {
        AfterSpawn[Index] = VirtualSlot(Site);
    }

    // RepairBuilding (its exec calls the slot, 0x160) comes two after NeedsRepair (21.40: NeedsRepair, GetCostToRepair,
    // RepairBuilding), which the improvement asks first.
    const int32_t RepairBuilding = ImplementationOf(L"/Script/FortniteGame.BuildingSMActor:RepairBuilding");
    const int32_t NeedsRepair = RepairBuilding >= 2 && Has(ImproveSlots, RepairBuilding - 2) ? RepairBuilding - 2 : -1;

    // The game mode scores Construction, Destruction, Upgrade, Repair and Edit in five slots in a row.
    const int32_t ScoreConstruction = Finder::ResolveScoreBuildingConstructionLive() ? Natives::AFortGameMode_ScoreBuildingConstructionIndex : -1;

    if (Candidates != 1 || PlaceCandidates != 1 || CanPerformAction < 0 || PermissionCandidates != 1 || GetEditing < 0 || !bReplaceSpawns || !bSetEditingStores || !bUpgradeReplaces
        || Die < 0 || SetMirrored < 0 || AfterSpawn[0] != SetMirrored || AfterSpawn[1] < 0 || AfterSpawn[2] < 0 || NeedsRepair < 0 || ScoreConstruction < 0)
    {
        Log::Error(L"Finder: the building RPCs' helpers not found (place 0x{:X}, improve 0x{:X}, begin edit 0x{:X}; afford {} x{}, place {} x{}, action {}, permission {} x{}; "
            L"edited {}, editing player {}, replace {}, set {}, upgrade {}; die {}, mirrored {}, after spawn {} {} {}, needs repair {}, score {})",
            Place ? Place - Finder::ImageBase() : 0, Improve ? Improve - Finder::ImageBase() : 0, BeginEdit ? BeginEdit - Finder::ImageBase() : 0, CanAfford, Candidates, CanPlace, PlaceCandidates,
            CanPerformAction, HasPermission, PermissionCandidates, IsBeingEdited, GetEditing, bReplaceSpawns, bSetEditingStores, bUpgradeReplaces, Die, SetMirrored, AfterSpawn[0], AfterSpawn[1],
            AfterSpawn[2], NeedsRepair, ScoreConstruction);
        return false;
    }

    Natives::AFortGameMode_ScoreBuildingRepairIndex = ScoreConstruction + 3;
    Natives::AFortGameMode_ScoreBuildingEditIndex = ScoreConstruction + 4;
    Natives::AFortPlayerController_CanPerformActionIndex = CanPerformAction;
    Natives::AFortPlayerController_HasPermissionToEditWorldIndex = HasPermission;
    Natives::AFortPlayerController_CanAffordToPlaceBuildableClassIndex = CanAfford;
    Natives::AFortPlayerController_CanAffordToRepairIndex = CanAfford + 1;
    Natives::AFortPlayerController_IsBuildingRestrictedIndex = CanAfford + 2;
    Natives::AFortPlayerController_HasRequiredBuildingLevelIndex = CanAfford + 3;
    Natives::AFortPlayerController_CanPlaceBuildableClassInStructuralGridIndex = CanPlace;
    Natives::AFortPlayerController_PayBuildableClassPlacementCostIndex = CanPlace + 1;
    Natives::AFortPlayerController_PayBuildingRepairCostIndex = CanPlace + 2;
    Natives::ABuildingActor_DieIndex = Die;
    Natives::ABuildingActor_InitializeBuildingActorIndex = AfterSpawn[1];
    Natives::ABuildingActor_PostInitializeSpawnedBuildingActorIndex = AfterSpawn[2];
    Natives::ABuildingSMActor_SetMirroredIndex = SetMirrored;
    Natives::ABuildingSMActor_OnServerAttemptBuildingUpgradeIndex = Upgrade;
    Natives::ABuildingSMActor_IsBeingPlayerEditedIndex = IsBeingEdited;
    Natives::ABuildingSMActor_ReplaceBuildingActorIndex = Replace;
    Natives::ABuildingSMActor_CanBePlayerEditedIndex = GetEditing - 2;
    Natives::ABuildingSMActor_GetEditingPlayerIndex = GetEditing;
    Natives::ABuildingSMActor_SetEditingPlayerIndex = SetEditing;
    Natives::ABuildingSMActor_NeedsRepairIndex = NeedsRepair;
    Natives::ABuildingSMActor_RepairBuildingIndex = RepairBuilding;
    Log::Info(L"Finder: building helpers: AFortPlayerController CanPerformAction 0x{:X}, HasPermissionToEditWorld 0x{:X}, CanAfford..HasRequiredBuildingLevel 0x{:X}-0x{:X}, "
        L"CanPlace..PayBuildingRepairCost 0x{:X}-0x{:X}; ABuildingSMActor Die 0x{:X}, Initialize 0x{:X} 0x{:X}, SetMirrored 0x{:X}, Upgrade 0x{:X}, IsBeingPlayerEdited 0x{:X}, "
        L"Replace 0x{:X}, CanBePlayerEdited 0x{:X}, EditingPlayer 0x{:X} 0x{:X}, NeedsRepair 0x{:X}, Repair 0x{:X}; AFortGameMode scores 0x{:X} 0x{:X} 0x{:X}",
        CanPerformAction, HasPermission, CanAfford, CanAfford + 3, CanPlace, CanPlace + 2, Die, AfterSpawn[1], AfterSpawn[2], SetMirrored, Upgrade, IsBeingEdited, Replace, GetEditing - 2,
        GetEditing, SetEditing, NeedsRepair, RepairBuilding, ScoreConstruction, ScoreConstruction + 3, ScoreConstruction + 4);
    return true;
}

bool Finder::OnlyLoadsEmoteAnimation(uintptr_t Implementation, ptrdiff_t AnimationOffset)
{
    // 1.9 (CL-3757339) and 1.9.1 (CL-3775276) keep only the first step of AFortPlayerController::
    // ServerPlayEmoteItem_Implementation, loading the emote's montage (EmoteAsset->Animation.LoadSynchronous()):
    // test rdx, rdx; jz; lea rcx, [rdx+Animation]; mov dl, 1; jmp (1.9 0x85B400, 1.9.1 0x85D9F0). The controller is gone
    // from rcx before the tail jump, so nothing after the load can reach it. 1.8.2 (0x84F000) keeps the controller and
    // calls the play body (0x82CD40) after the load. 1.10 (CL-3790078) strips the load too: its slot (0x1AD) holds the
    // shared empty function (0x1729B00), so Core loads the montage itself.
    return AnimationOffset > 0 && IsInText(Implementation)
        && ((FindInstruction(Implementation, Implementation + 15, { 0x48, 0x85, 0xD2, 0x74, -1, 0x48, 0x8D, 0x8A, -1, -1, -1, -1, 0xB2, 0x01, 0xE9 }) == Implementation
            && *reinterpret_cast<const int32_t*>(Implementation + 8) == AnimationOffset)
            || IsSharedEmptyBody(Implementation));
}

bool Finder::ResolveAbilitySpecLive()
{
    static bool bSearched = false;
    static bool bResolved = false;
    if (bSearched)
    {
        return bResolved;
    }
    bSearched = true;
    // Looked for where Core first builds a spec, not only by the emote restoration: 1.1 to 1.8.2 keep their emote body,
    // so that restoration never ran there and a player bot's first ability read the counter through null (CoreV2 #352,
    // 1.8.1). Called on its own or from another live resolver, whose count it leaves as it was.
    const int32_t MissingBefore = MissingCount;

    // The emote play body (1.8.2's) gives the pawn's ability system the emote ability with GiveAbilityAndActivateOnce
    // (1.8.1 0x3E4FC90, 1.8.2 0x3E58D50, 1.9 0x3E882C0, 1.9.1 0x3EA93F0), which the engine keeps.
    Require(L"UAbilitySystemComponent::GiveAbilityAndActivateOnce", Natives::UAbilitySystemComponent_GiveAbilityAndActivateOnce, FunctionOf(L"GiveAbilityAndActivateOnce called on ability %s on the client, not allowed!"));

    // The spec it builds takes a new handle inline (FGameplayAbilitySpecHandle::GenerateNewHandle: Handle = GHandleID++).
    // The constructor from a spec def does the same: mov eax, [GHandleID] ... inc [GHandleID] (1.8.1 0x3D814D6 and
    // 0x3D814DF, counter 0x51902BC; 1.8.2 0x3D8A2C6, 1.9 0x3DB9D56, 1.9.1 0x3DDA616).
    const uintptr_t SpecFromDef = FunctionOf(L"FGameplayAbilitySpec::FGameplayAbilitySpec for %s from %s");
    const uintptr_t SpecFromDefEnd = SpecFromDef ? Finder::FunctionEnd(SpecFromDef) : 0;
    uintptr_t HandleID = 0;
    for (uintptr_t Load = SpecFromDefEnd ? FindInstruction(SpecFromDef, SpecFromDefEnd, { 0x8B, 0x05 }) : 0; Load && !HandleID; Load = FindInstruction(Load + 1, SpecFromDefEnd, { 0x8B, 0x05 }))
    {
        const uintptr_t Counter = Finder::RelativeTarget(Load, 2, 6);
        const uintptr_t Increment = FindInstruction(Load + 6, std::min(Load + 0x20, SpecFromDefEnd), { 0xFF, 0x05 });
        if (Increment && !IsInText(Counter) && Finder::RelativeTarget(Increment, 2, 6) == Counter)
        {
            HandleID = Counter;
        }
        // 5.30 (CL-4305896) increments the loaded value and stores it back further on: mov eax, [GHandleID] (0x642195);
        // ...; inc eax; mov [GHandleID], eax (0x6421F3).
        const uintptr_t StoreEnd = std::min(Load + 0x80, SpecFromDefEnd);
        for (uintptr_t Store = !HandleID && !IsInText(Counter) ? FindInstruction(Load + 6, StoreEnd, { 0xFF, 0xC0, 0x89, 0x05 }) : 0; Store && !HandleID; Store = FindInstruction(Store + 1, StoreEnd, { 0xFF, 0xC0, 0x89, 0x05 }))
        {
            HandleID = Finder::RelativeTarget(Store + 2, 2, 6) == Counter ? Counter : 0;
        }
    }
    Require(L"FGameplayAbilitySpecHandle::GenerateNewHandle's counter", Natives::FGameplayAbilitySpecHandle_GHandleID, HandleID);

    bResolved = MissingCount == MissingBefore;
    MissingCount = MissingBefore;
    if (bResolved)
    {
        Log::Info(L"Finder: UAbilitySystemComponent::GiveAbilityAndActivateOnce 0x{:X}, FGameplayAbilitySpecHandle::GenerateNewHandle's counter 0x{:X}",
            reinterpret_cast<uintptr_t>(Natives::UAbilitySystemComponent_GiveAbilityAndActivateOnce) - Finder::ImageBase(), HandleID - Finder::ImageBase());
    }
    return bResolved;
}

bool Finder::ResolveEmoteLive(ptrdiff_t MyFortPawnOffset, ptrdiff_t StasisModeOffset)
{
    static bool bSearched = false;
    static bool bResolved = false;
    if (bSearched)
    {
        return bResolved;
    }
    bSearched = true;
    MissingCount = 0;

    // The play body gives the emote ability with GiveAbilityAndActivateOnce, in a spec with a new handle.
    if (!Finder::ResolveAbilitySpecLive())
    {
        ++MissingCount;
    }

    // The play body first asks CanPerformAction(Gameplay.Action.Player.InteractUse). AFortPlayerController's (1.8.2 slot
    // 0x26C 0x81D9E0, 1.9 0x26D 0x8297D0, 1.9.1 0x270 0x82BB30) asks the pawn, unless there is none or it is in stasis:
    // mov rcx, [rcx+MyFortPawn]; test rcx, rcx; jz; cmp byte [rcx+StasisMode], 0. It is the controller's only slot that
    // begins so (AFortPlayerControllerAthena overrides it for aiming down sights). The building restoration finds the
    // same slot another way where the build strips building.
    if (Natives::AFortPlayerController_CanPerformActionIndex < 0)
    {
        const int32_t P = static_cast<int32_t>(MyFortPawnOffset);
        const int32_t S = static_cast<int32_t>(StasisModeOffset);
        const int32_t CanPerformAction = MyFortPawnOffset > 0 && StasisModeOffset > 0 ? FirstVTableIndexWhere(CdoVTable(L"/Script/FortniteGame.FortPlayerController"), [P, S](uintptr_t Entry)
        {
            return FindInstruction(Entry, Entry + 1, { 0x48, 0x8B, 0x89, P & 0xFF, (P >> 8) & 0xFF, (P >> 16) & 0xFF, (P >> 24) & 0xFF, 0x48, 0x85, 0xC9, 0x74, -1,
                0x80, 0xB9, S & 0xFF, (S >> 8) & 0xFF, (S >> 16) & 0xFF, (S >> 24) & 0xFF, 0x00 }) == Entry;
        }) : -1;
        RequireSlot(L"AFortPlayerController::CanPerformAction", Natives::AFortPlayerController_CanPerformActionIndex, CanPerformAction);
    }

    Log::Info(L"Finder: emote helpers: GiveAbilityAndActivateOnce 0x{:X}, spec handle counter 0x{:X}, AFortPlayerController CanPerformAction 0x{:X}, {} missing",
        Natives::UAbilitySystemComponent_GiveAbilityAndActivateOnce ? reinterpret_cast<uintptr_t>(Natives::UAbilitySystemComponent_GiveAbilityAndActivateOnce) - Finder::ImageBase() : 0,
        Natives::FGameplayAbilitySpecHandle_GHandleID ? reinterpret_cast<uintptr_t>(Natives::FGameplayAbilitySpecHandle_GHandleID) - Finder::ImageBase() : 0,
        Natives::AFortPlayerController_CanPerformActionIndex, MissingCount);
    bResolved = MissingCount == 0;
    return bResolved;
}

bool Finder::ResolvePersonalVehicleLive(UFunction* IsPersonalVehicleAvailable, ptrdiff_t MainMcpProfileOffset)
{
    static bool bSearched = false;
    static bool bResolved = false;
    if (bSearched)
    {
        return bResolved;
    }
    bSearched = true;
    MissingCount = 0;

    // AFortPlayerController::IsPersonalVehicleAvailable (3.0 slot 0x2AB, 0xE3DEB0) is all one call:
    // sub rsp, 28h; call GetCurrentlySelectedPersonalVehicle; test rax, rax; setnz al. Its callee (0xE31910) begins by
    // reading the controller's MainMcpProfile: mov rdi, [rcx+MainMcpProfile]. 3.5 makes the callee virtual: mov rax, [rcx];
    // call [rax+1598h] (3.5.1 slot 0x2B3, 0xECD260), read from the controller's vtable.
    void** ControllerVTable = CdoVTable(L"/Script/FortniteGame.FortPlayerController");
    const int32_t AvailableIndex = ImplementationIndex(IsPersonalVehicleAvailable);
    const uintptr_t Available = VTableEntry(ControllerVTable, AvailableIndex);
    const uintptr_t Call = Available ? FindRawCall(Available, Available + 0x10) : 0;
    const uintptr_t VirtualCall = Available && !Call ? FindVirtualCall(Available, Available + 0x10) : 0;
    const uintptr_t Selected = Call ? Finder::RelativeTarget(Call, 1, 5) : VirtualCall ? VTableEntry(ControllerVTable, VirtualSlot(VirtualCall)) : 0;
    const int32_t M = static_cast<int32_t>(MainMcpProfileOffset);
    const bool bReadsProfile = Selected && M > 0
        && FindInstruction(Selected, Selected + 0x10, { 0x48, 0x8B, -1, M & 0xFF, (M >> 8) & 0xFF, (M >> 16) & 0xFF, (M >> 24) & 0xFF }) != 0;
    Require(L"AFortPlayerController::GetCurrentlySelectedPersonalVehicle", Natives::AFortPlayerController_GetCurrentlySelectedPersonalVehicle, bReadsProfile ? Selected : 0);

    Log::Info(L"Finder: personal vehicle helpers: IsPersonalVehicleAvailable slot 0x{:X}, GetCurrentlySelectedPersonalVehicle 0x{:X}, {} missing",
        AvailableIndex, bReadsProfile ? Selected - Finder::ImageBase() : 0, MissingCount);
    bResolved = MissingCount == 0;
    return bResolved;
}

bool Finder::ResolvePickupCanInteractLive(ptrdiff_t PickedUpOffset)
{
    if (Natives::IFortInteractInterface_CanInteractIndex >= 0)
    {
        return true;
    }

    // AFortPickup's IFortInteractInterface::CanInteract(const FInteractionType&) (1.8.1 0x69D9B0, 1.8.2 0x69DA10, and
    // AFortPickupAthena's 0x69DB30 / 0x69DB90) refuses anything but an IT_Simple interaction (cmp byte ptr [rdx], 1,
    // or the copy of rdx it keeps) and a pickup that is bPickedUp (cmp [this + bPickedUp - the interface's offset],
    // reg8) at its top. It is the first slot of the pickup's interface vtable to do both: slot 2 in 1.8.1 and 1.8.2,
    // after the destructor and _getUObject.
    if (!Natives::UObject_GetInterfaceAddress)
    {
        ResolveSpottingLive(FindObject<UClass>(nullptr, L"/Script/FortniteGame.FortSpottableActorInterface"));
    }
    UClass* InteractInterface = FindObject<UClass>(nullptr, L"/Script/FortniteGame.FortInteractInterface");
    UClass* PickupClass = FindObject<UClass>(nullptr, L"/Script/FortniteGame.FortPickup");
    UObject* PickupDefault = PickupClass ? PickupClass->GetDefaultObject() : nullptr;
    uint8_t* Interface = PickupDefault && InteractInterface && Natives::UObject_GetInterfaceAddress ? static_cast<uint8_t*>(Natives::UObject_GetInterfaceAddress(PickupDefault, InteractInterface)) : nullptr;
    void** InterfaceVTable = Interface ? *reinterpret_cast<void***>(Interface) : nullptr;
    const ptrdiff_t PickedUpDisplacement = Interface ? PickedUpOffset - (Interface - reinterpret_cast<uint8_t*>(PickupDefault)) : 0;

    int32_t CanInteractIndex = -1;
    for (int32_t Index = 2; InterfaceVTable && PickedUpDisplacement > 0 && Index < 0x10 && CanInteractIndex < 0; ++Index)
    {
        const uintptr_t Entry = VTableEntry(InterfaceVTable, Index);
        if (!Entry || Finder::IsStubFunction(Entry))
        {
            continue;
        }
        const uintptr_t EntryEnd = Finder::FunctionEnd(Entry);
        const uintptr_t ScanEnd = EntryEnd > Entry ? std::min(EntryEnd, Entry + 0x60) : Entry + 0x60;
        bool bChecksType = false;
        bool bChecksPickedUp = false;
        for (uintptr_t Site = Entry; Site + 6 <= ScanEnd; ++Site)
        {
            const uint8_t Opcode = Byte(Site);
            const uint8_t ModRm = Byte(Site + 1);
            const uint8_t Base = ModRm & 0x07;
            // cmp byte ptr [reg], 1
            bChecksType |= Opcode == 0x80 && (ModRm & 0xF8) == 0x38 && Base != 0x04 && Base != 0x05 && Byte(Site + 2) == 0x01;
            // cmp [reg + disp32], reg8 (or an imm8)
            bChecksPickedUp |= (Opcode == 0x38 || Opcode == 0x80) && (ModRm & 0xC0) == 0x80 && Base != 0x04 && *reinterpret_cast<const int32_t*>(Site + 2) == static_cast<int32_t>(PickedUpDisplacement);
        }
        CanInteractIndex = bChecksType && bChecksPickedUp ? Index : -1;
    }

    if (CanInteractIndex < 0)
    {
        Log::Error(L"Finder: AFortPickup's IFortInteractInterface::CanInteract not found (interface {}, bPickedUp at +0x{:X} from it)", Interface != nullptr, PickedUpDisplacement);
        return false;
    }
    Natives::IFortInteractInterface_CanInteractIndex = CanInteractIndex;
    Log::Info(L"Finder: IFortInteractInterface::CanInteract is slot {} (AFortPickup's interface at +0x{:X})", CanInteractIndex, Interface - reinterpret_cast<uint8_t*>(PickupDefault));
    return true;
}

bool Finder::ResolveServerOnAttemptInteractLive()
{
    if (Natives::IFortInteractInterface_ServerOnAttemptInteractIndex >= 0 && Natives::UObject_GetInterfaceAddress)
    {
        return true;
    }
    static bool bTried = false;
    if (bTried)
    {
        return false;
    }
    bTried = true;

    // IFortInteractInterface::execServerOnAttemptInteract reads the FInteractionType and ends in the interface's own
    // virtual, `call [rax+disp8]` on the vtable of the interface it is given (3.1 0x1051530: call [rax+28h], slot 5;
    // the wall's override there is 0xA86DB0).
    if (!Natives::UObject_GetInterfaceAddress)
    {
        ResolveSpottingLive(FindObject<UClass>(nullptr, L"/Script/FortniteGame.FortSpottableActorInterface"));
    }
    UFunction* Function = FindObject<UFunction>(nullptr, L"/Script/FortniteGame.FortInteractInterface:ServerOnAttemptInteract");
    const uintptr_t Exec = Function ? reinterpret_cast<uintptr_t>(Function->GetNativeFunc()) : 0;
    const uintptr_t ExecEnd = IsInText(Exec) ? Finder::FunctionEnd(Exec) : 0;
    int32_t Slot = -1;
    for (uintptr_t Site = ExecEnd ? FindInstruction(Exec, ExecEnd, { 0xFF, 0x50, -1 }) : 0; Site; Site = FindInstruction(Site + 1, ExecEnd, { 0xFF, 0x50, -1 }))
    {
        Slot = Byte(Site + 2) % 8 == 0 ? Byte(Site + 2) / 8 : -1;
    }
    if (Slot < 2 || !Natives::UObject_GetInterfaceAddress)
    {
        Log::Error(L"Finder: IFortInteractInterface::ServerOnAttemptInteract not found (exec {}, slot {}, GetInterfaceAddress {})", Exec != 0, Slot, Natives::UObject_GetInterfaceAddress != nullptr);
        return false;
    }
    Natives::IFortInteractInterface_ServerOnAttemptInteractIndex = Slot;
    Log::Info(L"Finder: IFortInteractInterface::ServerOnAttemptInteract is slot {}", Slot);
    return true;
}

bool Finder::ResolveItemCollectorGrantOutputLive(UObject* CollectorDefault, UClass* InteractInterface)
{
    if (Natives::ABuildingItemCollectorActor_GrantOutputDelegateVTable)
    {
        return true;
    }
    static bool bTried = false;
    if (bTried)
    {
        return false;
    }
    bTried = true;

    // ABuildingItemCollectorActor's IFortInteractInterface::ServerOnAttemptInteract (3.5.1 0xAA1F00) deposits first: its
    // first direct call (0xA87CD0), which takes the input and then calls CheckDepositGoalReached (0xA866C0). That one
    // sets a timer with a lambda's delegate instance, stored as `lea rax, [vtable]; mov [rdi], rax` twice: the functor
    // base's vtable (0x32CDE70), then the instance's own (0x32CDEB8). The instance's CreateCopy (slot 6, 0xA87F90) does
    // the same, so its last vtable is the instance's own; its Execute (slot 7, 0xA8A4E0) reads the three weak pointers
    // (three calls to FWeakObjectPtr::Get) and then only the controller's MyFortPawn: the GrantOutput call is gone. 6.00's
    // reads only the three weak pointers, the last one as a tail jmp.
    void* Interface = CollectorDefault && InteractInterface && ResolveServerOnAttemptInteractLive() ? Natives::UObject_GetInterfaceAddress(CollectorDefault, InteractInterface) : nullptr;
    void** InterfaceVTable = Interface ? *reinterpret_cast<void***>(Interface) : nullptr;
    const uintptr_t Interact = VTableEntry(InterfaceVTable, Natives::IFortInteractInterface_ServerOnAttemptInteractIndex);
    const uintptr_t InteractEnd = Interact ? Finder::FunctionEnd(Interact) : 0;
    const std::vector<std::pair<uintptr_t, uintptr_t>> InteractCalls = InteractEnd ? Finder::DirectCalls(Interact, InteractEnd) : std::vector<std::pair<uintptr_t, uintptr_t>>();
    const uintptr_t Deposit = InteractCalls.empty() ? 0 : InteractCalls.front().second;
    const uintptr_t DepositEnd = IsInText(Deposit) ? Finder::FunctionEnd(Deposit) : 0;

    const auto LeaTargets = [](uintptr_t Begin, uintptr_t End)
    {
        std::vector<uintptr_t> Targets;
        for (uintptr_t Lea = End ? FindInstruction(Begin, End, { 0x48, 0x8D, 0x05 }) : 0; Lea; Lea = FindInstruction(Lea + 1, End, { 0x48, 0x8D, 0x05 }))
        {
            Targets.push_back(Finder::RelativeTarget(Lea, 3, 7));
        }
        return Targets;
    };
    // 5.30 (CL-4305896) puts the instance's destructor in slot 6 (0x2DAD90, which stores the base vtable), so CreateCopy
    // is slot 7 (0xCAC440), Execute 8 (0xCAE910) and ExecuteIfSafe 9.
    const auto IsDelegateVTable = [&LeaTargets](uintptr_t Candidate, int32_t CreateCopyIndex)
    {
        void** VTable = reinterpret_cast<void**>(Candidate);
        if (!Candidate || IsInText(Candidate) || !Finder::IsReadable(VTable, (CreateCopyIndex + 3) * sizeof(void*)))
        {
            return false;
        }
        const uintptr_t CreateCopy = VTableEntry(VTable, CreateCopyIndex);
        const uintptr_t Execute = VTableEntry(VTable, CreateCopyIndex + 1);
        const uintptr_t CreateCopyEnd = CreateCopy ? Finder::FunctionEnd(CreateCopy) : 0;
        const uintptr_t ExecuteEnd = Execute ? Finder::FunctionEnd(Execute) : 0;
        if (!CreateCopyEnd || !ExecuteEnd || !VTableEntry(VTable, CreateCopyIndex + 2))
        {
            return false;
        }
        const std::vector<uintptr_t> CopyVTables = LeaTargets(CreateCopy, CreateCopyEnd);
        if (CopyVTables.empty() || CopyVTables.back() != Candidate)
        {
            return false;
        }
        // 6.00 (CL-4402180; 0xCF8AA0 on CL-4395664) drops the MyFortPawn read too, so the third FWeakObjectPtr::Get is
        // Execute's tail jmp: it counts with the calls.
        const auto TailJumpsTo = [Execute, ExecuteEnd](uintptr_t Target)
        {
            int32_t Jumps = 0;
            for (uintptr_t Jump = FindInstruction(Execute, ExecuteEnd, { 0xE9 }); Jump && Jump + 5 <= ExecuteEnd; Jump = FindInstruction(Jump + 1, ExecuteEnd, { 0xE9 }))
            {
                Jumps += Finder::RelativeTarget(Jump, 1, 5) == Target ? 1 : 0;
            }
            return Jumps;
        };
        int32_t MostCallsToOne = 0;
        const std::vector<std::pair<uintptr_t, uintptr_t>> ExecuteCalls = Finder::DirectCalls(Execute, ExecuteEnd);
        for (const auto& [Site, Target] : ExecuteCalls)
        {
            const int32_t Calls = static_cast<int32_t>(std::count_if(ExecuteCalls.begin(), ExecuteCalls.end(), [Target](const auto& Call) { return Call.second == Target; }));
            MostCallsToOne = std::max(MostCallsToOne, Calls == 2 ? Calls + TailJumpsTo(Target) : Calls);
        }
        return MostCallsToOne == 3;
    };

    uintptr_t CheckGoal = 0;
    uintptr_t DelegateVTable = 0;
    int32_t ExecuteIndex = -1;
    const std::vector<std::pair<uintptr_t, uintptr_t>> DepositCalls = DepositEnd ? Finder::DirectCalls(Deposit, DepositEnd) : std::vector<std::pair<uintptr_t, uintptr_t>>();
    for (const auto& [Site, Callee] : DepositCalls)
    {
        const uintptr_t CalleeEnd = IsInText(Callee) ? Finder::FunctionEnd(Callee) : 0;
        for (const uintptr_t Target : LeaTargets(Callee, CalleeEnd))
        {
            for (const int32_t CreateCopyIndex : { 6, 7 })
            {
                if (IsDelegateVTable(Target, CreateCopyIndex))
                {
                    CheckGoal = Callee;
                    DelegateVTable = Target;
                    ExecuteIndex = CreateCopyIndex + 1;
                    break;
                }
            }
        }
        if (DelegateVTable)
        {
            break;
        }
    }

    const auto Rva = [](uintptr_t Address) { return Address ? Address - Finder::ImageBase() : 0; };
    if (!DelegateVTable)
    {
        Log::Error(L"Finder: ABuildingItemCollectorActor's output timer not found (interact 0x{:X}, deposit 0x{:X}, {} calls)", Rva(Interact), Rva(Deposit), DepositCalls.size());
        return false;
    }
    Natives::ABuildingItemCollectorActor_GrantOutputDelegateVTable = reinterpret_cast<void**>(DelegateVTable);
    Natives::ABuildingItemCollectorActor_GrantOutputDelegateExecuteIndex = ExecuteIndex;
    Log::Info(L"Finder: ABuildingItemCollectorActor: interact 0x{:X}, deposit 0x{:X}, CheckDepositGoalReached 0x{:X}, output timer delegate vtable 0x{:X} (Execute slot {} 0x{:X})",
        Rva(Interact), Rva(Deposit), Rva(CheckGoal), Rva(DelegateVTable), ExecuteIndex, Rva(VTableEntry(reinterpret_cast<void**>(DelegateVTable), ExecuteIndex)));
    return true;
}

namespace
{
    // "Opcode ModRM disp32" at Site, addressing [base + disp32] (ModRM mod 10, no SIB byte), with ModRM's reg field Reg
    // (any when Reg < 0) and the displacement Displacement (any when it is < 0).
    bool IsDisp32Access(uintptr_t Site, uint8_t Opcode, int32_t Reg, ptrdiff_t Displacement)
    {
        const uint8_t ModRm = Byte(Site + 1);
        return Byte(Site) == Opcode && (ModRm & 0xC0) == 0x80 && (ModRm & 0x07) != 0x04 && (Reg < 0 || ((ModRm >> 3) & 0x07) == Reg)
            && (Displacement < 0 || *reinterpret_cast<const int32_t*>(Site + 2) == Displacement);
    }

    // The same, or its form with a base that needs a SIB byte (rsp, r12: "41 89 84 24 disp32" is mov [r12 + disp32], eax).
    bool IsDisp32AccessAnyBase(uintptr_t Site, uint8_t Opcode, int32_t Reg, ptrdiff_t Displacement)
    {
        const uint8_t ModRm = Byte(Site + 1);
        const bool bSib = Byte(Site) == Opcode && (ModRm & 0xC7) == 0x84 && Byte(Site + 2) == 0x24 && (Reg < 0 || ((ModRm >> 3) & 0x07) == Reg)
            && (Displacement < 0 || *reinterpret_cast<const int32_t*>(Site + 3) == Displacement);
        return bSib || IsDisp32Access(Site, Opcode, Reg, Displacement);
    }

    const wchar_t* const RemoveFromAlivePlayersLog = L"FortGameModeAthena: RemoveFromAlivePlayers called for Player [%s] who wasn't in the AlivePlayers list.";
}

bool Finder::ResolveMatchStatsLive()
{
    static bool bSearched = false;
    if (bSearched)
    {
        return Natives::UStatManager_GetStatValue != nullptr;
    }
    bSearched = true;

    // The match stats' analytics reader (1.10 0x39B410): "TotalDamage", "TotalKills", "TotalDowns" and "TotalRevives"
    // from the player state and from the controller's StatManager, whose values it asks UStatManager::GetStatValue
    // (0x9006E0: the stat, 0x900650, and its value in the period, rounded) for twice ("TeammateRevives", "DamageDealt",
    // period Map) before anything else it calls.
    for (const uintptr_t Ref : Finder::StringRefs(L"TotalRevives"))
    {
        const uintptr_t Function = Finder::FunctionStart(Ref);
        const uintptr_t End = Function ? Finder::FunctionEnd(Function) : 0;
        const std::vector<std::pair<uintptr_t, uintptr_t>> Calls = End ? Finder::DirectCalls(Function, End) : std::vector<std::pair<uintptr_t, uintptr_t>>();
        if (Calls.size() >= 2 && Calls[0].second == Calls[1].second && !Finder::IsStubFunction(Calls[0].second))
        {
            Natives::UStatManager_GetStatValue = reinterpret_cast<decltype(Natives::UStatManager_GetStatValue)>(Calls[0].second);
            break;
        }
    }
    // 4.4 strips that reader (0x9AD6E0 only builds its static names). GetStatValue (0x1045A60) then is the caller of the
    // stat lookup (0x10459D0, the first call of the "Attempt to modify stat" setter, 0x1047A60) that rounds the period's
    // value at once: call Lookup; ...; cvtss2si eax, xmm1; sar eax, 1 (FMath::RoundToInt).
    if (!Natives::UStatManager_GetStatValue)
    {
        const uintptr_t ModifyStat = FunctionOf(L"Attempt to modify stat '%s' failed as it cannot be found!");
        const uintptr_t ModifyStatEnd = ModifyStat ? Finder::FunctionEnd(ModifyStat) : 0;
        const std::vector<std::pair<uintptr_t, uintptr_t>> ModifyCalls = ModifyStatEnd ? Finder::DirectCalls(ModifyStat, ModifyStatEnd) : std::vector<std::pair<uintptr_t, uintptr_t>>();
        const uintptr_t Lookup = ModifyCalls.empty() ? 0 : ModifyCalls.front().second;
        uintptr_t GetStatValue = 0;
        int32_t Candidates = 0;
        for (const uintptr_t Site : Lookup ? Finder::CallsTo(Lookup, false) : std::vector<uintptr_t>())
        {
            const uintptr_t Function = Finder::FunctionStart(Site);
            if (!Function || Function == ModifyStat || Function == GetStatValue || Site - Function > 0x10)
            {
                continue;
            }
            const uintptr_t Round = FindInstruction(Site, Site + 0x40, { 0xF3, 0x0F, 0x2D, -1, 0xD1, 0xF8 });
            if (Round)
            {
                GetStatValue = Function;
                ++Candidates;
            }
        }
        if (Candidates == 1)
        {
            Natives::UStatManager_GetStatValue = reinterpret_cast<decltype(Natives::UStatManager_GetStatValue)>(GetStatValue);
        }
    }
    // 5.0 (UE 4.21) inlines the lookup too, and its GetStatValue (CL-4214610 0x11FEE20, no .pdata entry or padding) ends in
    // 4.5's rounding of the period's value: movsd xmm0, [rax+rcx*8]; cvtpd2ps; addss; subss; cvtss2si eax, xmm1; sar eax,
    // 1; neg eax; ret. Its start is the call target nearest before that tail, which dozens of callers share.
    if (!Natives::UStatManager_GetStatValue)
    {
        const std::initializer_list<int32_t> RoundedValue = { 0xF2, 0x0F, 0x10, 0x04, 0xC8, 0x66, 0x0F, 0x5A, 0xC0, 0xF3, 0x0F, 0x58, 0xC0, 0xF3, 0x0F, 0x5C, 0xC8, 0xF3, 0x0F, 0x2D, 0xC1, 0xD1, 0xF8, 0xF7, 0xD8, 0xC3 };
        const uintptr_t Tail = UniqueFunction(RoundedValue);
        std::unordered_map<uintptr_t, int32_t> Targets;
        for (uintptr_t Call = Tail ? FindInstruction(Finder::TextBegin(), Finder::TextEnd(), { 0xE8 }) : 0; Call; Call = FindInstruction(Call + 1, Finder::TextEnd(), { 0xE8 }))
        {
            const uintptr_t Target = Finder::RelativeTarget(Call, 1, 5);
            if (Target < Tail && Target + 0x100 > Tail)
            {
                ++Targets[Target];
            }
        }
        uintptr_t Start = 0;
        for (const auto& [Target, Count] : Targets)
        {
            if (Count >= 4 && Target > Start)
            {
                Start = Target;
            }
        }
        Natives::UStatManager_GetStatValue = reinterpret_cast<decltype(Natives::UStatManager_GetStatValue)>(Start);
    }
    if (!Natives::UStatManager_GetStatValue)
    {
        Log::Error(L"Finder: UStatManager::GetStatValue not found, the end-of-match screen gets no stats from the StatManager");
        return false;
    }
    Log::Info(L"Finder: UStatManager::GetStatValue 0x{:X}", reinterpret_cast<uintptr_t>(Natives::UStatManager_GetStatValue) - Finder::ImageBase());
    return true;
}

bool Finder::ResolveScoringEventLive()
{
    if (Natives::AFortPlayerStateAthena_HandleScoringEvent)
    {
        return true;
    }
    static bool bTried = false;
    if (bTried)
    {
        return false;
    }
    bTried = true;

    // Score Royale's scoring of a tagged event (5.30 0xC4B5A0, 5.40 0xC5A4F0): the player state's own function that counts
    // the event of each of the playlist's ScoringData entries whose EventInclusionTags the tags match, prices it (5.30
    // 0xC3C460) and adds what the player gained to TotalPlayerScore and the team's score (0xC3C540). Its callers are
    // AFortPlayerController::ServerAttemptInteract_Implementation (0x11C63C0, the receiving actor's own tags) and two
    // reflected natives that end in it with their actor's tags: AFortSpawnedScoreActor::OnScoreActorCollected (0xC51D70,
    // a coin's, for the player state it is given) and AFortAthenaSupplyDrop::SpawningLootOnDestruction (0xBC4650, a
    // supply drop's or llama's, for the pawn's player state). Both are its last call (5.40: 0xC61120 and 0xBCAF10).
    const auto LastCall = [](uintptr_t Function) -> uintptr_t
    {
        const uintptr_t End = IsInText(Function) ? Finder::FunctionEnd(Function) : 0;
        uintptr_t Target = 0;
        for (uintptr_t Call = End ? FindRawCall(Function, End) : 0; Call; Call = FindRawCall(Call + 1, End))
        {
            Target = Finder::RelativeTarget(Call, 1, 5);
        }
        return Target;
    };
    const uintptr_t FromCoin = LastCall(ExecImplementation(L"/Script/FortniteGame.FortSpawnedScoreActor:OnScoreActorCollected"));
    const uintptr_t FromSupplyDrop = LastCall(ExecImplementation(L"/Script/FortniteGame.FortAthenaSupplyDrop:SpawningLootOnDestruction"));
    if (!FromCoin || FromCoin != FromSupplyDrop)
    {
        Log::Error(L"Finder: AFortPlayerStateAthena::HandleScoringEvent not found (the coin's scoring calls 0x{:X}, the supply drop's 0x{:X})",
            FromCoin ? FromCoin - Finder::ImageBase() : 0, FromSupplyDrop ? FromSupplyDrop - Finder::ImageBase() : 0);
        return false;
    }
    Natives::AFortPlayerStateAthena_HandleScoringEvent = reinterpret_cast<decltype(Natives::AFortPlayerStateAthena_HandleScoringEvent)>(FromCoin);
    Log::Info(L"Finder: AFortPlayerStateAthena::HandleScoringEvent 0x{:X}", FromCoin - Finder::ImageBase());
    return true;
}

int32_t Finder::StrippedAthenaPawnDiedIndex()
{
    static bool bSearched = false;
    static int32_t StrippedIndex = -1;
    if (bSearched)
    {
        return StrippedIndex;
    }
    bSearched = true;

    // AFortPlayerControllerAthena's pawn-death handler overrides AFortPlayerControllerZone's (the only code reading the
    // "Deaths" stat name; 1.9 slot 0x305, 1.9.1 0x308) and calls it first. 1.9's (0x42DB20) then sets the death report
    // (DeathInfo), the time alive and the team's place, and takes the player out of the match with
    // AFortGameModeAthena::RemoveFromAlivePlayers. 1.9.1 (CL-3775276) strips all of it: its slot (0x42DA80) is
    // "jmp 0x895F90", Zone's handler, so RemoveFromAlivePlayers is left with one caller, Logout (0x42A7A0), and nobody
    // who dies is out of the match. 1.8.2 (slot 0x2FF, 0x42C460) and 1.9 keep their bodies.
    void** ZoneVTable = CdoVTable(L"/Script/FortniteGame.FortPlayerControllerZone");
    void** AthenaVTable = CdoVTable(L"/Script/FortniteGame.FortPlayerControllerAthena");
    if (!ZoneVTable || !AthenaVTable)
    {
        return -1;
    }
    uintptr_t Zone = 0;
    int32_t Index = -1;
    for (const uintptr_t Ref : Finder::StringRefs(L"Deaths"))
    {
        const uintptr_t Function = Finder::FunctionStart(Ref);
        Index = FirstVTableIndex(ZoneVTable, Function);
        if (Index >= 0)
        {
            Zone = Function;
            break;
        }
    }
    const uintptr_t Athena = VTableEntry(AthenaVTable, Index);
    if (!Zone || !Athena)
    {
        Log::Error(L"Finder: AFortPlayerControllerAthena's pawn-death handler not found (AFortPlayerControllerZone's 0x{:X}, slot {})", Zone ? Zone - Finder::ImageBase() : 0, Index);
        return -1;
    }

    // Stripped: the slot holds Zone's handler, a stub or a jump to Zone's, none of which calls RemoveFromAlivePlayers.
    const bool bJumpsToZone = Byte(Athena) == 0xE9 && Finder::RelativeTarget(Athena, 1, 5) == Zone;
    // 3.5 (3.5.1 0xA27A00, 3.5.2 0xA2B260, slot 0x337) keeps a body of its own, but only Zone's handler and, on the
    // server, the map marker's removal are left in it: "call AFortPlayerControllerZone's; cmp byte ptr [rbx + Role],
    // ROLE_Authority; jnz; ...; jmp 0xA1B160" (3.5.1: ServerRemoveMapCursor, then the player state's MapIndicatorPos
    // zeroed). RemoveFromAlivePlayers is left with Logout (3.5.1 0x9EE930) for its one caller, as on 1.9.1. Taken for the
    // build's own, nobody who died was out of the match or had a death report (no kill feed line), and the player bots'
    // win had no StartEndGamePhase to end the match with (CoreV2 #270).
    uintptr_t MapCursorRemoval = 0;
    const uintptr_t AthenaEnd = Finder::FunctionStart(Athena) == Athena ? Finder::FunctionEnd(Athena) : 0;
    const uintptr_t Remove = FunctionOf(RemoveFromAlivePlayersLog);
    // A short function that never calls RemoveFromAlivePlayers.
    const auto IsShortWithoutRemove = [Remove](uintptr_t Function)
    {
        const uintptr_t End = Function && Finder::FunctionStart(Function) == Function ? Finder::FunctionEnd(Function) : 0;
        bool bCallsRemove = false;
        for (const auto& [Site, Target] : End > Function && End - Function <= 0x80 ? Finder::DirectCalls(Function, End) : std::vector<std::pair<uintptr_t, uintptr_t>>())
        {
            bCallsRemove = bCallsRemove || Target == Remove;
        }
        return End > Function && End - Function <= 0x80 && Remove && !bCallsRemove;
    };
    const uintptr_t ZoneCall = AthenaEnd > Athena && AthenaEnd - Athena <= 0x60 ? FindRawCall(Athena, AthenaEnd) : 0;
    if (ZoneCall && Finder::RelativeTarget(ZoneCall, 1, 5) == Zone && !FindRawCall(ZoneCall + 5, AthenaEnd))
    {
        const uintptr_t Jump = FindInstruction(ZoneCall + 5, AthenaEnd, { 0xE9 });
        const uintptr_t Tail = Jump && Jump + 5 <= AthenaEnd ? Finder::RelativeTarget(Jump, 1, 5) : 0;
        MapCursorRemoval = IsShortWithoutRemove(Tail) ? Tail : 0;
    }
    // 5.0 (CL-4204761 0xBF8F10) first sends the player's inventory report (0xAE8310, a virtual that sends the
    // "Core.AthenaPlayerInventoryReport" analytics event), then calls Zone's handler, and on the server calls the map
    // marker's removal (0xBECBD0) and returns: "call 0xAE8310; ...; call Zone; cmp byte ptr [rsi + Role],
    // ROLE_Authority; jnz; mov rcx, rsi; call 0xBECBD0; ...; retn". Taken for the build's own, nobody who died was out
    // of the match and the player bots' win had no StartEndGamePhase (CoreV2 #349). Core's handler leaves the analytics
    // event out, as it does 1.9's "Combat.PlayerDeath".
    if (!MapCursorRemoval && AthenaEnd > Athena && AthenaEnd - Athena <= 0xA0)
    {
        const std::vector<std::pair<uintptr_t, uintptr_t>> Calls = Finder::DirectCalls(Athena, AthenaEnd);
        if (Calls.size() == 3 && Calls[0].second != Zone && Calls[1].second == Zone && Calls[0].second != Remove && Calls[2].second != Remove)
        {
            MapCursorRemoval = IsShortWithoutRemove(Calls[2].second) ? Calls[2].second : 0;
        }
    }
    const bool bStripped = Athena == Zone || bJumpsToZone || Finder::IsStubFunction(Athena) || MapCursorRemoval;
    if (!bStripped)
    {
        const uintptr_t AthenaEnd = Remove && Finder::FunctionStart(Athena) == Athena ? Finder::FunctionEnd(Athena) : 0;
        bool bCallsRemove = false;
        for (const auto& [Site, Target] : AthenaEnd > Athena ? Finder::DirectCalls(Athena, AthenaEnd) : std::vector<std::pair<uintptr_t, uintptr_t>>())
        {
            bCallsRemove = bCallsRemove || Target == Remove;
        }
        Log::Info(L"Finder: AFortPlayerControllerAthena's pawn-death handler is the build's own (slot 0x{:X}, 0x{:X}{})", Index, Athena - Finder::ImageBase(), bCallsRemove ? L", calls RemoveFromAlivePlayers" : L"");
        return -1;
    }

    Natives::AFortPlayerControllerZone_OnPawnDied = reinterpret_cast<decltype(Natives::AFortPlayerControllerZone_OnPawnDied)>(Zone);
    Natives::AFortPlayerControllerAthena_RemoveMapCursor = reinterpret_cast<decltype(Natives::AFortPlayerControllerAthena_RemoveMapCursor)>(MapCursorRemoval);
    StrippedIndex = Index;
    Log::Info(L"Finder: AFortPlayerControllerAthena's pawn-death handler is stripped: slot 0x{:X} holds 0x{:X} ({}AFortPlayerControllerZone's 0x{:X}{}), which never calls RemoveFromAlivePlayers",
        Index, Athena - Finder::ImageBase(), bJumpsToZone ? L"a jump to " : Athena == Zone ? L"" : MapCursorRemoval ? L"a call to " : L"a stub, not ", Zone - Finder::ImageBase(),
        MapCursorRemoval ? std::format(L" and the map marker's removal, 0x{:X}", MapCursorRemoval - Finder::ImageBase()) : std::wstring());
    return StrippedIndex;
}

int32_t Finder::StrippedAthenaPrepareDeathReportIndex(ptrdiff_t KillerPlayerStateOffset)
{
    static bool bSearched = false;
    static int32_t StrippedIndex = -1;
    if (bSearched)
    {
        return StrippedIndex;
    }
    bSearched = true;

    // AFortPlayerControllerZone's pawn-death handler (the "Deaths" reader, 1.10 0x8D6850) fills the death report with a
    // virtual call, PrepareDeathReport (1.10 slot 0x30C, 0x8DB450: "mov [rdi + PlayerDeathReport.KillerPlayerState],
    // rbx", the instigator's player state), before ClientOnPawnDied sends it. AFortPlayerControllerAthena overrides it:
    // Zone's, then the Athena game mode's part, of which 1.9.1 (0x431090) and 1.10 (0x449F80) keep only the cast's
    // "jmp AFortGameModeAthena::StaticClass" when the world has a game mode.
    void** ZoneVTable = CdoVTable(L"/Script/FortniteGame.FortPlayerControllerZone");
    void** AthenaVTable = CdoVTable(L"/Script/FortniteGame.FortPlayerControllerAthena");
    uintptr_t PawnDied = 0;
    for (const uintptr_t Ref : ZoneVTable && AthenaVTable ? Finder::StringRefs(L"Deaths") : std::vector<uintptr_t>())
    {
        const uintptr_t Function = Finder::FunctionStart(Ref);
        if (FirstVTableIndex(ZoneVTable, Function) >= 0)
        {
            PawnDied = Function;
            break;
        }
    }
    const uintptr_t PawnDiedEnd = PawnDied ? Finder::FunctionEnd(PawnDied) : 0;

    uintptr_t Zone = 0;
    uintptr_t Athena = 0;
    for (uintptr_t Site = PawnDiedEnd > PawnDied ? FindVirtualCall(PawnDied, PawnDiedEnd) : 0; Site && StrippedIndex < 0; Site = FindVirtualCall(Site + 1, PawnDiedEnd))
    {
        const int32_t Index = VirtualSlot(Site);
        Zone = VTableEntry(ZoneVTable, Index);
        Athena = VTableEntry(AthenaVTable, Index);
        if (!Zone || !Athena || Zone == Athena)
        {
            continue;
        }
        const uintptr_t ZoneEnd = Finder::FunctionStart(Zone) == Zone ? Finder::FunctionEnd(Zone) : 0;
        bool bWritesKiller = false;
        for (uintptr_t Store = Zone; ZoneEnd && Store + 6 <= ZoneEnd && !bWritesKiller; ++Store)
        {
            bWritesKiller = IsDisp32Access(Store, 0x89, -1, KillerPlayerStateOffset);
        }
        if (!bWritesKiller)
        {
            continue;
        }

        // Stripped: a short body whose one call is Zone's report, and which ends in the jump.
        const uintptr_t AthenaEnd = Finder::FunctionStart(Athena) == Athena ? Finder::FunctionEnd(Athena) : 0;
        const uintptr_t Call = AthenaEnd > Athena && AthenaEnd - Athena <= 0x80 ? FindRawCall(Athena, AthenaEnd) : 0;
        const bool bCallsZoneOnly = Call && Finder::RelativeTarget(Call, 1, 5) == Zone && !FindRawCall(Call + 5, AthenaEnd);
        const bool bJumps = bCallsZoneOnly && FindInstruction(Call + 5, AthenaEnd, { 0xE9 });
        Log::Info(L"Finder: AFortPlayerControllerAthena::PrepareDeathReport is slot 0x{:X}, 0x{:X} (AFortPlayerControllerZone's 0x{:X}){}", Index, Athena - Finder::ImageBase(),
            Zone - Finder::ImageBase(), bJumps ? L": its Athena game mode part is stripped" : L"");
        if (!bJumps)
        {
            return -1;
        }
        StrippedIndex = Index;
    }
    if (StrippedIndex < 0)
    {
        return -1;
    }
    Natives::AFortPlayerControllerZone_PrepareDeathReport = reinterpret_cast<decltype(Natives::AFortPlayerControllerZone_PrepareDeathReport)>(Zone);
    return StrippedIndex;
}

bool Finder::ResolveAthenaPawnDiedLive(ptrdiff_t PlaceOffset, ptrdiff_t SecondsAliveOffset, ptrdiff_t DeathCauseOffset, ptrdiff_t DeathInfoDBNOOffset, ptrdiff_t TeamMembersOffset)
{
    static bool bSearched = false;
    static bool bResolved = false;
    if (bSearched)
    {
        return bResolved;
    }
    bSearched = true;

    const auto Rva = [](uintptr_t Address) { return Address ? Address - Finder::ImageBase() : 0; };
    const auto CallsOf = [](uintptr_t Function)
    {
        const uintptr_t End = Function && Finder::FunctionStart(Function) == Function ? Finder::FunctionEnd(Function) : 0;
        return End > Function ? Finder::DirectCalls(Function, End) : std::vector<std::pair<uintptr_t, uintptr_t>>();
    };

    // AFortGameModeAthena::RemoveFromAlivePlayers (1.9.1 0x4325C0, 1.9 0x433200) logs this, and its last call is the solo
    // win (1.9.1 0x438B50, after "is the last player alive.  Starting end game.").
    const uintptr_t Remove = FunctionOf(RemoveFromAlivePlayersLog);
    const std::vector<std::pair<uintptr_t, uintptr_t>> RemoveCalls = CallsOf(Remove);
    uintptr_t SoloWin = RemoveCalls.empty() ? 0 : RemoveCalls.back().second;

    // The team's place setter (1.9.1 0x435C20, 1.9 0x436600): the win's "mov edx, 1" call (place 1 for the winners). It
    // sets Place on each of TeamMembers and on the controller's player state through the player state's setter (1.9.1
    // 0x47AAD0: "mov [rcx + Place], edx").
    const auto WritesPlace = [PlaceOffset](uintptr_t Function)
    {
        for (uintptr_t Site = Function; Site < Function + 0x20; ++Site)
        {
            if (IsDisp32Access(Site, 0x89, -1, PlaceOffset))
            {
                return true;
            }
        }
        return false;
    };
    const auto FindSetTeamPlace = [&](uintptr_t Win)
    {
        const uintptr_t WinEnd = Win && Finder::FunctionStart(Win) == Win ? Finder::FunctionEnd(Win) : 0;
        for (uintptr_t Site = WinEnd ? FindInstruction(Win, WinEnd, { 0xBA, 0x01, 0x00, 0x00, 0x00 }) : 0; Site; Site = FindInstruction(Site + 1, WinEnd, { 0xBA, 0x01, 0x00, 0x00, 0x00 }))
        {
            const uintptr_t Call = FindRawCall(Site + 5, std::min(WinEnd, Site + 0x10));
            const uintptr_t Target = Call ? Finder::RelativeTarget(Call, 1, 5) : 0;
            for (const auto& [CalleeSite, Callee] : CallsOf(Target))
            {
                if (WritesPlace(Callee))
                {
                    return Target;
                }
            }
        }
        return uintptr_t{ 0 };
    };
    uintptr_t SetTeamPlace = FindSetTeamPlace(SoloWin);

    // 3.6 puts AFortGameModeAthena::CheckWinConditionLastManStanding (0x9EB8B0) between them: RemoveFromAlivePlayers'
    // last call is now the check, which ends the match through StartEndGamePhaseTeam (0xA09BA0, "Team %d won the
    // match!") in a large team game, else StartEndGamePhase (0xA099C0, "%s won the match!") for the last player alive.
    // StartEndGamePhase is the old solo win (place 1, the time alive, the end game): the check's last direct call.
    if (!SetTeamPlace && SoloWin)
    {
        const std::vector<std::pair<uintptr_t, uintptr_t>> CheckCalls = CallsOf(SoloWin);
        uintptr_t StartEndGamePhase = CheckCalls.empty() ? 0 : CheckCalls.back().second;
        // 5.10 (CL-4240749) calls another function (0xBC6940) after StartEndGamePhase (0xBDCA50): take the call to the
        // function that logs the win, if the check makes one.
        const uintptr_t WonTheMatch = FunctionOf(L"FortGameModeAthena: %s won the match!");
        for (const auto& [CheckSite, Callee] : WonTheMatch ? CheckCalls : std::vector<std::pair<uintptr_t, uintptr_t>>())
        {
            StartEndGamePhase = Callee == WonTheMatch ? Callee : StartEndGamePhase;
        }
        SetTeamPlace = FindSetTeamPlace(StartEndGamePhase);
        if (SetTeamPlace)
        {
            Log::Info(L"Finder: RemoveFromAlivePlayers ends in CheckWinConditionLastManStanding (0x{:X}), whose StartEndGamePhase is 0x{:X}", Rva(SoloWin), Rva(StartEndGamePhase));
            SoloWin = StartEndGamePhase;
        }
    }
    const uintptr_t SoloWinEnd = SoloWin && Finder::FunctionStart(SoloWin) == SoloWin ? Finder::FunctionEnd(SoloWin) : 0;

    // The time alive (1.9.1 0x4634D0, 1.9 0x464250): the win stores what it returns in SecondsAlive. 1.9.1 then counts
    // what the winner outlived (the native +0xE10, which the player state's constructor sets to -1): the match's starting
    // player count (the game state's +0x13D8, copied from PlayersLeft as the aircraft starts) less the winner's teammates
    // who are out (0x424C40: the team's size at the start, +0xE14, less those still standing, less 1) and 1:
    // "call 0x424C40; mov ecx, [rbx + 13D8h]; sub ecx, eax; dec ecx; mov [rsi + 0E10h], ecx". 1.9 counts no teammates.
    uintptr_t SecondsAliveStore = 0;
    for (uintptr_t Site = SoloWin; SoloWinEnd && Site + 6 <= SoloWinEnd && !SecondsAliveStore; ++Site)
    {
        SecondsAliveStore = IsDisp32Access(Site, 0x89, 0, SecondsAliveOffset) ? Site : 0;
    }
    uintptr_t GetSecondsAlive = 0;
    for (uintptr_t Call = SecondsAliveStore ? FindRawCall(SecondsAliveStore - 0x10, SecondsAliveStore) : 0; Call; Call = FindRawCall(Call + 1, SecondsAliveStore))
    {
        GetSecondsAlive = Finder::RelativeTarget(Call, 1, 5);
    }
    const uintptr_t CountCall = SecondsAliveStore ? FindRawCall(SecondsAliveStore + 6, SecondsAliveStore + 0xC) : 0;
    uintptr_t StartingPlayersLoad = 0;
    for (uintptr_t Site = CountCall ? CountCall + 5 : 0; Site && Site < CountCall + 0xC && !StartingPlayersLoad; ++Site)
    {
        StartingPlayersLoad = IsDisp32Access(Site, 0x8B, -1, -1) ? Site : 0;
    }
    uintptr_t OutlivedStore = 0;
    for (uintptr_t Site = StartingPlayersLoad ? StartingPlayersLoad + 6 : 0; Site && Site < StartingPlayersLoad + 0x14 && !OutlivedStore; ++Site)
    {
        OutlivedStore = IsDisp32Access(Site, 0x89, -1, -1) ? Site : 0;
    }
    // 5.21 (CL-4288479) counts no teammates out: the win stores the starting players less the team's size at the start
    // (a native of the player state the aircraft's start sets, 0xBE27B0), "mov eax, [rsi + 1030h]; sub eax, [rbx + 0F84h];
    // mov [rbx + 0F80h], eax" (0xBE57EA), and the team placement (0xBE59B0, from CheckWinConditionLastManStanding) does
    // the same for each player of the team still at -1. The game counts what the player outlived itself there.
    bool bOutlivedByGame = false;
    if (SecondsAliveStore && !CountCall && IsDisp32Access(SecondsAliveStore + 6, 0x8B, -1, -1) && Byte(SecondsAliveStore + 12) == 0x2B && (Byte(SecondsAliveStore + 13) & 0xC0) == 0x80 &&
        IsDisp32Access(SecondsAliveStore + 18, 0x89, -1, -1) && Byte(SecondsAliveStore + 7) >> 3 == Byte(SecondsAliveStore + 13) >> 3 && Byte(SecondsAliveStore + 13) == Byte(SecondsAliveStore + 19))
    {
        StartingPlayersLoad = SecondsAliveStore + 6;
        OutlivedStore = SecondsAliveStore + 18;
        bOutlivedByGame = true;
    }

    // The death cause (1.9.1 0x43AE20, on the pawn): 1.9 matched the damage tags in the handler itself, 1.9.1 has a
    // function for it, which its DBNO handling (AFortPlayerPawnAthena's 0x438640) calls with the damage tags and false,
    // then stores with bDBNO: "call 0x43AE20; mov [rbp + DeathInfo.DeathCause], al; mov byte ptr [rbp + DeathInfo.bDBNO],
    // 1". It returns FallDamage or OutsideSafeZone (DBNOTimeout for those when the pawn was DBNO), DBNOTimeout for a
    // bleed-out, the weapon's or the trap's cause, else Unspecified.
    // From 3.5 the function is the player state's static ToDeathCause (21.40's name), which takes only the tags and
    // bWasDBNO: the DBNO handling passes them in rcx and edx, "lea rcx, [rdi + tags]; ...; xor edx, edx; call" (3.5.1
    // 0xA31A09, 3.6 0xA3F339, 4.0 0xAA20B9, 4.1 0xAAE53C), where the pawn's took the tags in rdx and false in r8.
    void** PawnVTable = CdoVTable(L"/Script/FortniteGame.FortPlayerPawnAthena");
    uintptr_t GetDeathCause = 0;
    bool bStaticDeathCause = false;
    FirstVTableIndexWhere(PawnVTable, [&](uintptr_t Entry)
    {
        const uintptr_t End = Finder::FunctionStart(Entry) == Entry ? Finder::FunctionEnd(Entry) : 0;
        for (uintptr_t Site = Entry; End && Site + 0x18 <= End; ++Site)
        {
            if (Byte(Site) != 0xE8 || !IsInText(Finder::RelativeTarget(Site, 1, 5)))
            {
                continue;
            }
            // 2.2 (0x46E3B8) zeroes a register first: call; xor ebx, ebx; mov [r14 + DeathCause], al.
            const uintptr_t Cleared = Site + 5 + ((Byte(Site + 5) == 0x33 || Byte(Site + 5) == 0x31) && (Byte(Site + 6) & 0xC0) == 0xC0 ? 2 : 0);
            const uintptr_t Store = IsDisp32Access(Cleared, 0x88, 0, DeathCauseOffset) ? Cleared : IsDisp32Access(Cleared + 1, 0x88, 0, DeathCauseOffset) ? Cleared + 1 : 0;
            // 3.6 (0xA3F338) and 4.0 (0xAA20B8) zero DeathInfo.Distance between the two: call; xor ebx, ebx;
            // mov [r14 + DeathCause], al; mov [r14 + Distance], ebx; mov byte ptr [r14 + bDBNO], 1.
            const uintptr_t NextStore = Store && Byte(Store + 6) == 0x41 && IsDisp32Access(Store + 7, 0x89, -1, DeathCauseOffset + 3) ? Store + 7 : Store;
            if (Store && (IsDisp32Access(NextStore + 6, 0xC6, 0, DeathInfoDBNOOffset) || IsDisp32Access(NextStore + 7, 0xC6, 0, DeathInfoDBNOOffset)))
            {
                GetDeathCause = Finder::RelativeTarget(Site, 1, 5);
                bStaticDeathCause = (Byte(Site - 2) == 0x33 || Byte(Site - 2) == 0x31) && Byte(Site - 1) == 0xD2;
                return true;
            }
            // 4.4 (0xAD5AC2) builds the FDeathInfo on the stack and hands it to the player state's setter (0xAC4BB0):
            // xor edx, edx; call ToDeathCause; mov byte ptr [rsp + Info.DeathCause], al; ...;
            // mov byte ptr [rsp + Info.bDBNO], 1.
            // 4.5 (CL-4166199 0xAAC6FB) clears edx before the stack stores of the other fields and asks for the new
            // DeathCircumstance right after: xor edx, edx; ...; call ToDeathCause (0xAF8250); mov rcx, rdi;
            // mov byte ptr [rsp + Info.DeathCause], al; call 0xAF8BF0; mov [rsp + Info.DeathCircumstance], eax; ...;
            // mov byte ptr [rsp + Info.bDBNO], 1.
            bool bClearsFlag = false;
            for (uintptr_t Clear = Site - 2; Clear + 0x30 >= Site && !bClearsFlag; --Clear)
            {
                bClearsFlag = (Byte(Clear) == 0x33 || Byte(Clear) == 0x31) && Byte(Clear + 1) == 0xD2;
            }
            const uintptr_t CauseStore = Site + 5 + ((Byte(Site + 5) & 0xF8) == 0x48 && Byte(Site + 6) == 0x8B && (Byte(Site + 7) & 0xF8) == 0xC8 ? 3 : 0);
            if (bClearsFlag && Byte(CauseStore) == 0x88 && Byte(CauseStore + 1) == 0x44 && Byte(CauseStore + 2) == 0x24
                && DeathInfoDBNOOffset + 1 == DeathCauseOffset)
            {
                const uint8_t CauseSlot = Byte(CauseStore + 3);
                for (uintptr_t Flag = CauseStore + 4; Flag + 5 <= CauseStore + 0x24; ++Flag)
                {
                    if (Byte(Flag) == 0xC6 && Byte(Flag + 1) == 0x44 && Byte(Flag + 2) == 0x24 && Byte(Flag + 3) == static_cast<uint8_t>(CauseSlot - 1) && Byte(Flag + 4) == 0x01)
                    {
                        GetDeathCause = Finder::RelativeTarget(Site, 1, 5);
                        bStaticDeathCause = true;
                        return true;
                    }
                }
            }
        }
        return false;
    });

    // The controller's TeamMembers fill (1.9.1 slot 0x331, 0x41F270; 1.9 0x32E), which the handler calls first in squads:
    // when TeamMembers is empty ("cmp dword ptr [rcx + TeamMembers.Num], 0"), it adds the game state's player states and
    // the game mode's inactive ones that are on the controller's team.
    void** AthenaVTable = CdoVTable(L"/Script/FortniteGame.FortPlayerControllerAthena");
    const ptrdiff_t TeamMembersNumOffset = TeamMembersOffset + 8;
    const int32_t UpdateTeamMembersIndex = TeamMembersOffset > 0 ? FirstVTableIndexWhere(AthenaVTable, [TeamMembersNumOffset](uintptr_t Entry)
    {
        bool bChecksEmpty = false;
        for (uintptr_t Site = Entry; Site < Entry + 0x20 && !bChecksEmpty; ++Site)
        {
            bChecksEmpty = IsDisp32Access(Site, 0x83, 7, TeamMembersNumOffset) && Byte(Site + 6) == 0;
        }
        const uintptr_t End = bChecksEmpty && Finder::FunctionStart(Entry) == Entry ? Finder::FunctionEnd(Entry) : 0;
        for (uintptr_t Site = Entry; End && Site + 6 <= End; ++Site)
        {
            if (IsDisp32AccessAnyBase(Site, 0x89, -1, TeamMembersNumOffset))
            {
                return true;
            }
        }
        return false;
    }) : -1;

    // 3.1 dropped the fill and its SquadMembers sibling (3.0 slots 0x351 and 0x352): the game mode keeps TeamMembers
    // itself, appending each player state to its teammates' controllers as it joins a team. A game mode virtual (3.1
    // 0x9C7410: the player state's dedicated-server flag, then the team join) calls 0x9A6F10, which appends inline:
    // "movsxd rsi, [r15 + TeamMembers.Num]; ...; mov [r15 + TeamMembers.Num], ecx". Without the fill, the handler needs
    // nothing more there.
    // 3.6 and 4.0 add through a function of the controller's own (3.6 0xA1CD80, 4.0 0xA45A60: "add rcx, TeamMembers;
    // call" TArray's add, then the sort) that the team join (3.6 0x9E6DD0, 4.0 0xA11950) calls, one call further down
    // from the game mode's virtual (4.0 slot 0x155, 0xA269D0).
    int32_t GameModeTeamJoinIndex = -1;
    if (UpdateTeamMembersIndex < 0 && TeamMembersOffset > 0)
    {
        const auto AddsTeamMember = [TeamMembersOffset, TeamMembersNumOffset](uintptr_t Callee)
        {
            const uintptr_t End = Finder::FunctionStart(Callee) == Callee ? Finder::FunctionEnd(Callee) : 0;
            for (uintptr_t Write = Callee; End && Write + 6 <= End; ++Write)
            {
                if (IsDisp32AccessAnyBase(Write, 0x89, -1, TeamMembersNumOffset))
                {
                    return true;
                }
            }
            for (uintptr_t Add = Callee; End && Add + 7 <= End && Add < Callee + 0x30; ++Add)
            {
                if (Byte(Add) == 0x48 && Byte(Add + 1) == 0x81 && Byte(Add + 2) == 0xC1 && *reinterpret_cast<const int32_t*>(Add + 3) == TeamMembersOffset && FindRawCall(Add + 7, Add + 0xC))
                {
                    return true;
                }
            }
            return false;
        };
        void** GameModeVTable = CdoVTable(L"/Script/FortniteGame.FortGameModeAthena");
        GameModeTeamJoinIndex = FirstVTableIndexWhere(GameModeVTable, [&](uintptr_t Entry)
        {
            for (const auto& [Site, Callee] : CallsOf(Entry))
            {
                if (AddsTeamMember(Callee))
                {
                    return true;
                }
            }
            return false;
        });
        GameModeTeamJoinIndex = GameModeTeamJoinIndex >= 0 ? GameModeTeamJoinIndex : FirstVTableIndexWhere(GameModeVTable, [&](uintptr_t Entry)
        {
            for (const auto& [Site, Callee] : CallsOf(Entry))
            {
                for (const auto& [InnerSite, InnerCallee] : CallsOf(Callee))
                {
                    if (AddsTeamMember(InnerCallee))
                    {
                        return true;
                    }
                }
            }
            return false;
        });
        // 4.4 adds inline in the team join (0xA711C0, called by the game mode's virtual 0xA95DE0 after the player
        // state's dedicated-server flag): "lea rcx, [r14 + TeamMembers]; call" TArray's AddUnique, then the sort.
        const auto AddsTeamMemberInline = [TeamMembersOffset](uintptr_t Callee)
        {
            const uintptr_t End = Finder::FunctionStart(Callee) == Callee ? Finder::FunctionEnd(Callee) : 0;
            for (uintptr_t Lea = Callee; End && Lea + 12 <= End; ++Lea)
            {
                if ((Byte(Lea) & 0xFE) == 0x48 && Byte(Lea + 1) == 0x8D && (Byte(Lea + 2) & 0xF8) == 0x88 && (Byte(Lea + 2) & 0x07) != 0x04
                    && *reinterpret_cast<const int32_t*>(Lea + 3) == TeamMembersOffset && Byte(Lea + 7) == 0xE8)
                {
                    return true;
                }
            }
            return false;
        };
        GameModeTeamJoinIndex = GameModeTeamJoinIndex >= 0 ? GameModeTeamJoinIndex : FirstVTableIndexWhere(GameModeVTable, [&](uintptr_t Entry)
        {
            for (const auto& [Site, Callee] : CallsOf(Entry))
            {
                if (AddsTeamMemberInline(Callee))
                {
                    return true;
                }
            }
            return false;
        });
    }

    if (!Remove || !SetTeamPlace || !GetSecondsAlive)
    {
        Log::Error(L"Finder: the pawn-death handler's pieces not found (RemoveFromAlivePlayers 0x{:X}, solo win 0x{:X}, team place 0x{:X}, seconds alive 0x{:X})",
            Rva(Remove), Rva(SoloWin), Rva(SetTeamPlace), Rva(GetSecondsAlive));
        return false;
    }

    Natives::AFortGameModeAthena_RemoveFromAlivePlayers = reinterpret_cast<decltype(Natives::AFortGameModeAthena_RemoveFromAlivePlayers)>(Remove);
    Natives::AFortGameModeAthena_StartEndGamePhase = reinterpret_cast<decltype(Natives::AFortGameModeAthena_StartEndGamePhase)>(SoloWin);
    Natives::AFortPlayerControllerAthena_SetTeamPlace = reinterpret_cast<decltype(Natives::AFortPlayerControllerAthena_SetTeamPlace)>(SetTeamPlace);
    Natives::AFortPlayerStateAthena_GetSecondsAlive = reinterpret_cast<decltype(Natives::AFortPlayerStateAthena_GetSecondsAlive)>(GetSecondsAlive);
    if (bStaticDeathCause)
    {
        Natives::AFortPlayerStateAthena_ToDeathCause = reinterpret_cast<decltype(Natives::AFortPlayerStateAthena_ToDeathCause)>(GetDeathCause);
    }
    else
    {
        Natives::AFortPlayerPawn_GetDeathCause = reinterpret_cast<decltype(Natives::AFortPlayerPawn_GetDeathCause)>(GetDeathCause);
    }
    Natives::AFortPlayerControllerAthena_UpdateTeamMembersIndex = UpdateTeamMembersIndex;
    if (OutlivedStore)
    {
        // Left unset where the game counts it (bOutlivedByGame), so the restored handler leaves the count to the game.
        Natives::AFortPlayerControllerAthena_CountTeammatesOut = CountCall ? reinterpret_cast<decltype(Natives::AFortPlayerControllerAthena_CountTeammatesOut)>(Finder::RelativeTarget(CountCall, 1, 5)) : nullptr;
        Natives::AFortGameStateAthena_StartingPlayers = *reinterpret_cast<const int32_t*>(StartingPlayersLoad + 2);
        Natives::AFortPlayerStateAthena_PlayersOutlived = *reinterpret_cast<const int32_t*>(OutlivedStore + 2);
    }
    bResolved = true;

    Log::Info(L"Finder: pawn-death handler: RemoveFromAlivePlayers 0x{:X}, team place 0x{:X} (solo win 0x{:X}), seconds alive 0x{:X}, death cause 0x{:X}{}, TeamMembers fill slot 0x{:X}",
        Rva(Remove), Rva(SetTeamPlace), Rva(SoloWin), Rva(GetSecondsAlive), Rva(GetDeathCause), bStaticDeathCause ? L" (ToDeathCause)" : L"", UpdateTeamMembersIndex);
    Log::Info(L"Finder: pawn-death handler: teammates out 0x{:X}, starting players +0x{:X}, players outlived +0x{:X}",
        Rva(reinterpret_cast<uintptr_t>(Natives::AFortPlayerControllerAthena_CountTeammatesOut)), Natives::AFortGameStateAthena_StartingPlayers, Natives::AFortPlayerStateAthena_PlayersOutlived);
    if (bOutlivedByGame)
    {
        Log::Info(L"Finder: pawn-death handler: the win and the team placement count what each player outlived (the starting players less the team's size at the start, +0x{:X})",
            *reinterpret_cast<const int32_t*>(SecondsAliveStore + 14));
    }
    if (GameModeTeamJoinIndex >= 0)
    {
        Log::Info(L"Finder: pawn-death handler: no TeamMembers fill; the game mode fills TeamMembers as players join teams (slot 0x{:X})", GameModeTeamJoinIndex);
    }
    const bool bTeamMembersFillMissing = UpdateTeamMembersIndex < 0 && GameModeTeamJoinIndex < 0;
    if (!GetDeathCause || bTeamMembersFillMissing || !OutlivedStore)
    {
        Log::Warning(L"Finder: pawn-death handler: {}{}{}not found, the restored handler goes without",
            GetDeathCause ? L"" : L"the death cause, ", bTeamMembersFillMissing ? L"the TeamMembers fill, " : L"", OutlivedStore ? L"" : L"the outlived count, ");
    }
    return true;
}

bool Finder::ResolveSetDeathInfoLive(ptrdiff_t DeathInfoOffset, ptrdiff_t DeathCauseOffset, ptrdiff_t InitializedOffset, ptrdiff_t DeathCircumstanceOffset)
{
    static bool bSearched = false;
    if (bSearched)
    {
        return Natives::AFortPlayerStateAthena_SetDeathInfo != nullptr;
    }
    bSearched = true;

    // The pawn's DBNO handling (AFortPlayerPawnAthena 6.02 CL-4461277 0xC5B8A0, 6.01 CL-4424678 0xC59030) builds the
    // knock's FDeathInfo on its stack and hands it to the player state's store:
    //   xor edx, edx; call ToDeathCause; mov rcx, rdi; mov [rsp + Info.DeathCause], al;
    //   call <the death circumstance, of the pawn in rcx>; mov [rsp + Info.DeathCircumstance], eax; ...;
    //   mov byte ptr [rsp + Info.bDBNO], 1; ...; lea rdx, [rsp + Info]; mov rcx, r13; call <the store>
    // (6.02 0xC5BE23 to 0xC5BF7C, 6.01 0xC595B3 to 0xC5970C). The store (6.02 0xC4A090, 6.01 0xC478B0) begins with the
    // copy over the player state's ("movups [rcx + DeathInfo], xmm0") and "mov byte ptr [rcx + DeathInfo.bInitialized],
    // 1": only a callee that has both is taken. The circumstance's function (6.02 0xC5F320, 6.01 0xC5CAB0) tests the
    // pawn it is given for null before anything else ("test rcx, rcx").
    const auto Rva = [](uintptr_t Address) { return Address ? Address - Finder::ImageBase() : 0; };
    const uintptr_t ToDeathCause = reinterpret_cast<uintptr_t>(Natives::AFortPlayerStateAthena_ToDeathCause);
    const ptrdiff_t CauseInInfo = DeathCauseOffset - DeathInfoOffset;
    const ptrdiff_t CircumstanceInInfo = DeathCircumstanceOffset > DeathInfoOffset ? DeathCircumstanceOffset - DeathInfoOffset : -1;
    const auto IsStore = [DeathInfoOffset, InitializedOffset](uintptr_t Callee)
    {
        bool bCopies = false;
        bool bInitializes = false;
        for (uintptr_t Site = Callee; IsInText(Callee) && IsInText(Callee + 0x80) && Site < Callee + 0x60; ++Site)
        {
            // movups [rcx + DeathInfo], xmm
            bCopies = bCopies || (Byte(Site) == 0x0F && Byte(Site + 1) == 0x11 && (Byte(Site + 2) & 0xC7) == 0x81 && *reinterpret_cast<const int32_t*>(Site + 3) == DeathInfoOffset);
            // mov byte ptr [rcx + DeathInfo.bInitialized], 1
            bInitializes = bInitializes || (IsDisp32Access(Site, 0xC6, 0, InitializedOffset) && (Byte(Site + 1) & 0x07) == 0x01 && Byte(Site + 6) == 0x01);
        }
        return bCopies && bInitializes;
    };
    uintptr_t Handling = 0;
    uintptr_t SetDeathInfo = 0;
    uintptr_t GetDeathCircumstance = 0;
    int32_t InfoSlot = -1;
    void** PawnVTable = ToDeathCause && DeathInfoOffset > 0 && InitializedOffset > DeathInfoOffset && CauseInInfo > 0 && CauseInInfo < 0x40 ? CdoVTable(L"/Script/FortniteGame.FortPlayerPawnAthena") : nullptr;
    FirstVTableIndexWhere(PawnVTable, [&](uintptr_t Entry)
    {
        const uintptr_t End = Finder::FunctionStart(Entry) == Entry ? Finder::FunctionEnd(Entry) : 0;
        for (const auto& [Site, Target] : End > Entry ? Finder::DirectCalls(Entry, End) : std::vector<std::pair<uintptr_t, uintptr_t>>())
        {
            if (Target != ToDeathCause || Site + 0x20 > End)
            {
                continue;
            }
            // mov rcx, r64 (the pawn, for the circumstance's call), then the cause's store on the stack.
            const bool bMovesPawn = (Byte(Site + 5) & 0xFE) == 0x48 && Byte(Site + 6) == 0x8B && (Byte(Site + 7) & 0xF8) == 0xC8;
            const uintptr_t CauseStore = Site + 5 + (bMovesPawn ? 3 : 0);
            const int32_t Slot = Byte(CauseStore) == 0x88 && Byte(CauseStore + 1) == 0x44 && Byte(CauseStore + 2) == 0x24 && Byte(CauseStore + 3) < 0x80
                ? static_cast<int32_t>(Byte(CauseStore + 3)) - static_cast<int32_t>(CauseInInfo) : -1;
            if (Slot < 0)
            {
                continue;
            }
            // lea rdx, [rsp + Info]; (mov rcx, r64;) call
            uintptr_t Store = 0;
            for (uintptr_t Lea = FindInstruction(CauseStore + 4, std::min(End, CauseStore + 0x400), { 0x48, 0x8D, 0x54, 0x24, Slot }); Lea && !Store;
                Lea = FindInstruction(Lea + 1, std::min(End, CauseStore + 0x400), { 0x48, 0x8D, 0x54, 0x24, Slot }))
            {
                const bool bMovesState = (Byte(Lea + 5) & 0xFE) == 0x48 && Byte(Lea + 6) == 0x8B && (Byte(Lea + 7) & 0xF8) == 0xC8;
                const uintptr_t Call = Lea + 5 + (bMovesState ? 3 : 0);
                const uintptr_t Callee = Call + 5 <= End && Byte(Call) == 0xE8 ? Finder::RelativeTarget(Call, 1, 5) : 0;
                Store = Callee && IsStore(Callee) ? Callee : 0;
            }
            if (!Store)
            {
                continue;
            }
            Handling = Entry;
            SetDeathInfo = Store;
            InfoSlot = Slot;
            // call; mov [rsp + Info.DeathCircumstance], eax
            const uintptr_t CircumstanceCall = CauseStore + 4;
            const uintptr_t Circumstance = bMovesPawn && CircumstanceInInfo > 0 && Byte(CircumstanceCall) == 0xE8 ? Finder::RelativeTarget(CircumstanceCall, 1, 5) : 0;
            if (Circumstance && Circumstance != ToDeathCause && IsInText(Circumstance) && IsInText(Circumstance + 0x40) && Byte(CircumstanceCall + 5) == 0x89 && Byte(CircumstanceCall + 6) == 0x44
                && Byte(CircumstanceCall + 7) == 0x24 && Byte(CircumstanceCall + 8) < 0x80 && static_cast<int32_t>(Byte(CircumstanceCall + 8)) == Slot + static_cast<int32_t>(CircumstanceInInfo)
                && FindInstruction(Circumstance, Circumstance + 0x20, { 0x48, 0x85, 0xC9 }))
            {
                GetDeathCircumstance = Circumstance;
            }
            return true;
        }
        return false;
    });
    if (!SetDeathInfo)
    {
        Log::Error(L"Finder: the player state's death store not found (ToDeathCause 0x{:X}, DeathInfo +0x{:X}, bInitialized +0x{:X}): a player's death is stored field by field, without bInitialized, so the game's own respawn code doesn't move a respawned player to its respawn place",
            Rva(ToDeathCause), DeathInfoOffset > 0 ? DeathInfoOffset : 0, InitializedOffset > 0 ? InitializedOffset : 0);
        return false;
    }
    Natives::AFortPlayerStateAthena_SetDeathInfo = reinterpret_cast<decltype(Natives::AFortPlayerStateAthena_SetDeathInfo)>(SetDeathInfo);
    Natives::AFortPlayerPawnAthena_GetDeathCircumstance = reinterpret_cast<decltype(Natives::AFortPlayerPawnAthena_GetDeathCircumstance)>(GetDeathCircumstance);
    Log::Info(L"Finder: the player state's death store is 0x{:X} (the pawn's DBNO handling 0x{:X} hands it the FDeathInfo at rsp+0x{:X}; it sets bInitialized +0x{:X}), the death circumstance 0x{:X}",
        Rva(SetDeathInfo), Rva(Handling), InfoSlot, InitializedOffset, Rva(GetDeathCircumstance));
    if (!GetDeathCircumstance && CircumstanceInInfo > 0)
    {
        Log::Warning(L"Finder: the death circumstance's function not found, a death is stored with the circumstance it had");
    }
    return true;
}

bool Finder::ResolveCreateUniquePlayerIdLive(void* const* OnlineEngineInterfaceVTable)
{
    static bool bSearched = false;
    if (bSearched || Natives::UOnlineEngineInterface_CreateUniquePlayerIdIndex >= 0)
    {
        return Natives::UOnlineEngineInterface_CreateUniquePlayerIdIndex >= 0;
    }
    bSearched = true;

    // UOnlineEngineInterfaceImpl::CreateUniquePlayerId(const FString& Str) (3.1 0x316930, slot 0x268; 2.5's 0x30ADB0 and
    // 3.0's 0x30F660 are the same code): "mov rbx, r8" keeps Str, Online::GetIdentityInterface, then
    // "mov r8, rbx; mov rdx, rsi; call [rax + CreateUniquePlayerId]" on the identity. GetUniquePlayerId (the next slot)
    // passes LocalUserNum instead, and GetPlayerNickname (after it) keeps its id from r9 ("mov rbx, r9").
    // UE 4.20's CreateUniquePlayerId(const FString& Str, FName Type) (3.3 0x328DE0, slot 0x278) keeps Str in rbp ("mov
    // rbp, r8"), asks IsLoaded(Type) ("mov rdx, r9; mov rax, [rcx]; call [rax+238h]"), then Online::GetIdentityInterface
    // and "mov r8, rbp; call [rax + CreateUniquePlayerId]" on the identity. Without it 3.3's bots had no ids, and clients,
    // which file names under ids, showed one name for every bot (#247).
    for (int32_t Index = 0x40; OnlineEngineInterfaceVTable && Index < 0x60; ++Index)
    {
        const uintptr_t Function = reinterpret_cast<uintptr_t>(OnlineEngineInterfaceVTable[Index]);
        if (!IsInText(Function) || !IsInText(Function + 0x80))
        {
            continue;
        }
        const uintptr_t KeepString = FindInstruction(Function, Function + 0x30, { 0x49, 0x8B, 0xD8 });
        uintptr_t Call = KeepString ? FindInstruction(KeepString, Function + 0x50, { 0x4C, 0x8B, 0xC3, 0x48, 0x8B, 0xD6, 0xFF, 0x90 }) : 0;
        const uintptr_t KeepTypedString = Call ? 0 : FindInstruction(Function, Function + 0x30, { 0x49, 0x8B, 0xE8 });
        const uintptr_t IsLoaded = KeepTypedString ? FindInstruction(KeepTypedString, KeepTypedString + 0x20, { 0x49, 0x8B, 0xD1, 0x48, 0x8B, 0x01, 0xFF, 0x90 }) : 0;
        Call = IsLoaded ? FindInstruction(IsLoaded, Function + 0x80, { 0x4C, 0x8B, 0xC5, 0xFF, 0x90 }) : Call;
        // 5.0 (CL-4204761 0x3FACD0, the same slot) keeps Str in r14 ("mov r14, r8"), asks IsLoaded(Type) with "mov rax,
        // [rcx]; mov rdx, r9; call [rax+238h]", then "mov r8, r14; call [rax+0D0h]" on the identity. Without it 5.0's
        // bots had no ids again, and one name on clients (CoreV2 #349).
        const uintptr_t KeepR14String = Call ? 0 : FindInstruction(Function, Function + 0x30, { 0x4D, 0x8B, 0xF0 });
        const uintptr_t IsLoadedR14 = KeepR14String ? FindInstruction(KeepR14String, KeepR14String + 0x20, { 0x48, 0x8B, 0x01, 0x49, 0x8B, 0xD1, 0xFF, 0x90 }) : 0;
        Call = IsLoadedR14 ? FindInstruction(IsLoadedR14, Function + 0x80, { 0x4D, 0x8B, 0xC6, 0xFF, 0x90, 0xD0, 0x00, 0x00, 0x00 }) : Call;
        if (Call)
        {
            Natives::UOnlineEngineInterface_CreateUniquePlayerIdIndex = Index;
            Log::Info(L"Finder: UOnlineEngineInterface::CreateUniquePlayerId is slot 0x{:X} (0x{:X})", Index * 8, Function - Finder::ImageBase());
            return true;
        }
    }
    Log::Warning(L"Finder: UOnlineEngineInterface::CreateUniquePlayerId not found, player bots go without ids");
    return false;
}

bool Finder::ResolveNextPlayerIDLive(ptrdiff_t PlayerIdOffset)
{
    static bool bSearched = false;
    if (bSearched || Natives::AGameSession_NextPlayerID)
    {
        return Natives::AGameSession_NextPlayerID != nullptr;
    }
    bSearched = true;

    // AGameSession::RegisterPlayer(APlayerController* NewPlayer, const TSharedPtr<const FUniqueNetId>& UniqueId, bool
    // bWasFromInvite) (3.1 0x2056E60, slot 0x608; 3.3 0x20DC8E0 and 3.5.1 0x217C620, slot 0x610) with GetNextPlayerID
    // inlined: "mov eax, cs:NextPlayerID; ...; inc eax; mov cs:NextPlayerID, eax; mov rax, [rdx + PlayerState];
    // mov [rax + PlayerId], ecx", then SetUniqueId and RegisterPlayerWithSession on the player state.
    void** SessionVTable = CdoVTable(L"/Script/Engine.GameSession");
    for (int32_t Index = 0x60; SessionVTable && PlayerIdOffset > 0 && Index < 0x110; ++Index)
    {
        const uintptr_t Function = reinterpret_cast<uintptr_t>(SessionVTable[Index]);
        if (!IsInText(Function) || !IsInText(Function + 0x60))
        {
            continue;
        }
        uintptr_t Counter = 0;
        bool bStored = false;
        bool bPlayerId = false;
        for (uintptr_t Site = Function; Site < Function + 0x50; ++Site)
        {
            // A load and a store of one rip-relative int (modrm 00 reg 101), and the store to the player state's PlayerId.
            const bool bRipRelative = (Byte(Site + 1) & 0xC7) == 0x05;
            if (!Counter && Byte(Site) == 0x8B && bRipRelative)
            {
                Counter = Finder::RelativeTarget(Site, 2, 6);
            }
            else if (Counter && Byte(Site) == 0x89 && bRipRelative && Finder::RelativeTarget(Site, 2, 6) == Counter)
            {
                bStored = true;
            }
            else if (bStored && Byte(Site) == 0x89 && (Byte(Site + 1) & 0xC0) == 0x80 && *reinterpret_cast<const int32_t*>(Site + 2) == static_cast<int32_t>(PlayerIdOffset))
            {
                bPlayerId = true;
                break;
            }
        }
        if (bPlayerId && !IsInText(Counter))
        {
            Natives::AGameSession_NextPlayerID = reinterpret_cast<int32_t*>(Counter);
            Log::Info(L"Finder: AGameSession::RegisterPlayer is slot 0x{:X} (0x{:X}), its NextPlayerID 0x{:X} ({})", Index * 8, Function - Finder::ImageBase(), Counter - Finder::ImageBase(),
                *Natives::AGameSession_NextPlayerID);
            return true;
        }
    }
    Log::Warning(L"Finder: AGameSession::RegisterPlayer's NextPlayerID not found, player bots go without PlayerIds");
    return false;
}

bool Finder::ResolveTeamPrivateHealthAndShieldLive(ptrdiff_t PlayerTeamPrivateOffset, ptrdiff_t MaxShieldOffset)
{
    static bool bSearched = false;
    if (bSearched || Natives::AFortTeamPrivateInfo_SetHealthAndShield)
    {
        return Natives::AFortTeamPrivateInfo_SetHealthAndShield != nullptr;
    }
    bSearched = true;

    // AFortGameModeAthena::ReplicateHealthAndShield (3.3 0x9FE870 slot 0xB08; 3.5.1 0x9F7440, 3.6 0xA03C40 and 4.0
    // 0xA2B8A0, slot 0xB10), each tick: for every player state, a player controller's pawn's health and shield onto
    // CurrentHealth, MaxHealth, CurrentShield and MaxShield, else "mov dword ptr [rbx + MaxShield], 42C80000h" (100), and
    // then the team's copy: "mov rcx, [rbx + PlayerTeamPrivate]; test rcx, rcx; jz; mov rdx, rbx; call
    // AFortTeamPrivateInfo::SetHealthAndShield" (3.5.1 0xF83CB0, 21.40's name). That copy (RepData, a fast array) is what a
    // client's squad bar reads for a squadmate (AFortTeamPrivateInfo's 3.5.1 0xF703D0 reads the player state only for its
    // own local player).
    void** GameModeVTable = CdoVTable(L"/Script/FortniteGame.FortGameModeAthena");
    for (int32_t Index = 0x120; GameModeVTable && PlayerTeamPrivateOffset > 0 && MaxShieldOffset > 0 && Index < 0x1A0; ++Index)
    {
        const uintptr_t Function = reinterpret_cast<uintptr_t>(GameModeVTable[Index]);
        if (!IsInText(Function) || !IsInText(Function + 0x300))
        {
            continue;
        }
        bool bMaxShield = false;
        for (uintptr_t Site = Function; Site < Function + 0x300; ++Site)
        {
            // mov dword ptr [reg + MaxShield], 100.0f (C7 /0, modrm 10 000 reg).
            if (Byte(Site) == 0xC7 && (Byte(Site + 1) & 0xF8) == 0x80 && *reinterpret_cast<const int32_t*>(Site + 2) == static_cast<int32_t>(MaxShieldOffset)
                && *reinterpret_cast<const uint32_t*>(Site + 6) == 0x42C80000)
            {
                bMaxShield = true;
            }
            // mov rcx, [reg + PlayerTeamPrivate]; test rcx, rcx; jz short; mov rdx, reg; call rel32.
            const uint8_t ModRM = Byte(Site + 2);
            if (!bMaxShield || Byte(Site) != 0x48 || Byte(Site + 1) != 0x8B || (ModRM & 0xF8) != 0x88 || (ModRM & 7) == 4
                || *reinterpret_cast<const int32_t*>(Site + 3) != static_cast<int32_t>(PlayerTeamPrivateOffset) || Byte(Site + 7) != 0x48 || Byte(Site + 8) != 0x85
                || Byte(Site + 9) != 0xC9 || Byte(Site + 10) != 0x74 || Byte(Site + 12) != 0x48 || Byte(Site + 13) != 0x8B || Byte(Site + 14) != (0xD0 | (ModRM & 7))
                || Byte(Site + 15) != 0xE8)
            {
                continue;
            }
            const uintptr_t SetHealthAndShield = Finder::RelativeTarget(Site + 15, 1, 5);
            if (!IsInText(SetHealthAndShield))
            {
                break;
            }
            Natives::AFortTeamPrivateInfo_SetHealthAndShield = reinterpret_cast<decltype(Natives::AFortTeamPrivateInfo_SetHealthAndShield)>(SetHealthAndShield);
            Log::Info(L"Finder: AFortGameModeAthena::ReplicateHealthAndShield is slot 0x{:X} (0x{:X}), its AFortTeamPrivateInfo::SetHealthAndShield 0x{:X}", Index * 8,
                Function - Finder::ImageBase(), SetHealthAndShield - Finder::ImageBase());
            return true;
        }
    }
    Log::Warning(L"Finder: AFortTeamPrivateInfo::SetHealthAndShield not found, player bots' squad bar health stays empty");
    return false;
}

bool Finder::ResolveReplicationDriverLive()
{
    static bool bSearched = false;
    if (bSearched || Natives::UReplicationDriver_ServerReplicateActorsIndex >= 0)
    {
        return Natives::UReplicationDriver_ServerReplicateActorsIndex >= 0;
    }
    bSearched = true;

    // UReplicationDriver::ServerReplicateActors is PURE_VIRTUAL: the base class's body only reports "Pure virtual not
    // implemented (UReplicationDriver::ServerReplicateActors)", so that name's function is the one in its default object's
    // vtable, and its slot is the one UReplicationGraph overrides.
    const uintptr_t PureVirtual = FunctionOf(L"UReplicationDriver::ServerReplicateActors");
    const int32_t Index = Finder::VTableIndex(CdoVTable(L"/Script/Engine.ReplicationDriver"), reinterpret_cast<void*>(PureVirtual), 0x400);
    if (Index < 0)
    {
        Log::Warning(L"Finder: UReplicationDriver::ServerReplicateActors not found, a replication driver replicates nothing");
        return false;
    }
    Natives::UReplicationDriver_ServerReplicateActorsIndex = Index;
    Log::Info(L"Finder: UReplicationDriver::ServerReplicateActors is slot 0x{:X} (0x{:X})", Index * 8, PureVirtual - Finder::ImageBase());
    return true;
}

bool Finder::ResolveReplicationGraphActorInfoLive(void** GraphVTable)
{
    static bool bSearched = false;
    if (bSearched || Natives::FPerConnectionActorInfoMap_RemoveByKey)
    {
        return Natives::FPerConnectionActorInfoMap_RemoveByKey != nullptr;
    }
    bSearched = true;

    // CoreV2 #411. UE 4.21's replication graph keeps, for each connection, an info for every actor it replicated
    // (UNetReplicationGraphConnection's ActorInfoMap: ActorMap by the actor's address, then ChannelMap by its channel,
    // both TMap<pointer, TSharedPtr<FConnectionReplicationActorInfo>>). UReplicationGraph::NotifyActorTearOff (6.00
    // CL-4395664 0x7D5A40) sets the info's bTearOff (+0x1D bit 1), ReplicateSingleActor (0x7DF520) closes the channel
    // right after replicating whenever that is set, and nothing ever takes the entry out again: NotifyActorChannelCleanedUp
    // (0x7D5060) only removes the ChannelMap's and clears Channel and the frame counters, RemoveNetworkActor (0x7DE420)
    // only the graph's own maps. An actor spawned at a dead, torn off actor's address finds that entry and is opened and
    // closed on that connection every time it replicates. Later engines end RemoveNetworkActor with the missing step
    // (8.30 0x8E1C70: for each of Connections, ActorMap.Remove(Actor), a call of the same remove-by-key as the
    // ChannelMap's, 0x38A1C0), which UReplicationGraph::RemoveActorInfoFromConnections does here with the build's own.
    //
    // That function is found as NotifyActorChannelCleanedUp's second call, and nothing is resolved unless the code
    // around it is what 6.00, 6.01 (CL-4424678 0x78A1D0, 0x78ABB0), 6.02 (CL-4461277 0x78AD60, 0x78B740), 6.10
    // (CL-4480234 0x7A3920, 0x7A4300) and 6.20 (CL-4497486 0x740FE0, 0x7419D0) have:
    //   NotifyActorChannelCleanedUp: lea r14, [rcx+80h] (the ChannelMap); call FindId; ...; mov rdx, rsi (the channel);
    //     mov rcx, r14; call Remove; mov [rdi], r15; mov [rdi+0Ch], r15 (the info's Channel and its frame counters);
    //     then the rest of the info it clears, which says how big the info is and where its flags are:
    //       6.00 to 6.10: mov [rdi+14h], r15 (a 0x20 byte info, flags at +0x1D)
    //       6.20: mov [rdi+24h], r15d; mov [rdi+18h], r15 (a 0x30 byte info, flags at +0x28, as 8.30's)
    //   NotifyActorTearOff: lea rcx, [rsi+30h] (the ActorMap, 0x50 before the ChannelMap: one TMap); call the same
    //     FindId; ...; or byte ptr [rax+<the flags>], 2
    // 6.20's RemoveNetworkActor (0x74A3D0) still doesn't take the connections' infos out. A build before 6.00 or after
    // 6.20 was never looked at, and one whose code differs (8.30's engine does this itself) keeps what it has.
    constexpr uint32_t FirstVerifiedChangelist = 4395664;
    constexpr uint32_t LastVerifiedChangelist = 4497486;
    constexpr ptrdiff_t MapSize = 0x50;
    const uint32_t Changelist = Packed::Changelist();
    const auto LeftAsIs = [Changelist](const wchar_t* Why)
    {
        Log::Warning(L"Finder: the replication graph's per-connection actor infos are left as CL-{} has them ({}), so a pawn spawned at a dead pawn's address may not reach its player (#411)", Changelist, Why);
        return false;
    };
    if (Changelist < FirstVerifiedChangelist || Changelist > LastVerifiedChangelist)
    {
        return LeftAsIs(L"only 6.00 to 6.20, CL-4395664 to CL-4497486, were checked");
    }

    const std::vector<uintptr_t> CleanedUpRefs = Finder::StringRefs(L"UNetReplicationGraphConnection_NotifyActorChannelCleanedUp");
    const uintptr_t CleanedUp = CleanedUpRefs.size() == 1 ? Finder::FunctionStart(CleanedUpRefs[0]) : 0;
    const uintptr_t CleanedUpEnd = CleanedUp ? Finder::FunctionEnd(CleanedUp) : 0;
    if (!CleanedUp || CleanedUpEnd <= CleanedUp || CleanedUpEnd - CleanedUp > 0x400)
    {
        return LeftAsIs(L"NotifyActorChannelCleanedUp not found");
    }

    const std::vector<std::pair<uintptr_t, uintptr_t>> CleanedUpCalls = Finder::DirectCalls(CleanedUp, CleanedUpEnd);
    if (CleanedUpCalls.size() != 3)
    {
        return LeftAsIs(L"NotifyActorChannelCleanedUp doesn't make its three calls");
    }
    // lea r64, [rcx+disp32], before the first call: the ChannelMap, kept in a register for the two calls.
    ptrdiff_t ChannelMapOffset = -1;
    int32_t MapRegister = -1;
    for (uintptr_t Address = CleanedUp; Address + 7 <= CleanedUpCalls[0].first; ++Address)
    {
        const uint8_t* Code = reinterpret_cast<const uint8_t*>(Address);
        if ((Code[0] == 0x48 || Code[0] == 0x4C) && Code[1] == 0x8D && (Code[2] & 0xC7) == 0x81)
        {
            if (ChannelMapOffset >= 0)
            {
                return LeftAsIs(L"NotifyActorChannelCleanedUp takes more than one member's address");
            }
            ChannelMapOffset = *reinterpret_cast<const int32_t*>(Code + 3);
            MapRegister = ((Code[2] >> 3) & 7) | (Code[0] == 0x4C ? 8 : 0);
        }
    }
    const uintptr_t FindId = CleanedUpCalls[0].second;
    const uintptr_t RemoveSite = CleanedUpCalls[1].first;
    const uintptr_t Remove = CleanedUpCalls[1].second;
    const uint8_t* BeforeRemove = reinterpret_cast<const uint8_t*>(RemoveSite - 6);
    const uint8_t* AfterRemove = reinterpret_cast<const uint8_t*>(RemoveSite + 5);
    // mov rdx, r64 (the key, by value); mov rcx, <the map's register>.
    const bool bRemoveArguments = RemoveSite - 6 > CleanedUpCalls[0].first && (BeforeRemove[0] == 0x48 || BeforeRemove[0] == 0x49) && BeforeRemove[1] == 0x8B && (BeforeRemove[2] & 0xF8) == 0xD0
        && (BeforeRemove[3] == 0x48 || BeforeRemove[3] == 0x49) && BeforeRemove[4] == 0x8B && (BeforeRemove[5] & 0xF8) == 0xC8
        && ((BeforeRemove[5] & 7) | (BeforeRemove[3] == 0x49 ? 8 : 0)) == MapRegister;
    // mov [r+Displacement], r64 (REX.W) or r32 (a REX without W: r8d to r15d); [r] when Displacement is 0.
    const auto IsStore = [](const uint8_t* Code, bool bQword, uint8_t Displacement)
    {
        return (Code[0] & 0xF8) == (bQword ? 0x48 : 0x40) && Code[1] == 0x89 && (Displacement ? (Code[2] & 0xC0) == 0x40 && Code[3] == Displacement : (Code[2] & 0xC0) == 0x00);
    };
    // The found info's Channel and frame counters cleared, and the rest of what is cleared, which says where its flags are.
    ptrdiff_t InfoFlagsOffset = -1;
    if (RemoveSite + 5 + 7 <= CleanedUpEnd && IsStore(AfterRemove, true, 0) && IsStore(AfterRemove + 3, true, 0x0C))
    {
        if (RemoveSite + 5 + 11 <= CleanedUpEnd && IsStore(AfterRemove + 7, true, 0x14))
        {
            InfoFlagsOffset = 0x1D;
        }
        else if (RemoveSite + 5 + 15 <= CleanedUpEnd && IsStore(AfterRemove + 7, false, 0x24) && IsStore(AfterRemove + 11, true, 0x18))
        {
            InfoFlagsOffset = 0x28;
        }
    }
    if (ChannelMapOffset < MapSize || !bRemoveArguments || InfoFlagsOffset < 0 || !FindId || !Remove || Remove == FindId)
    {
        return LeftAsIs(L"NotifyActorChannelCleanedUp isn't 6.00's or 6.20's");
    }

    // UReplicationGraph::NotifyActorTearOff: the graph's override of the driver's pure virtual of that name.
    const std::vector<uintptr_t> TearOffRefs = Finder::StringRefs(L"UReplicationDriver::NotifyActorTearOff");
    const uintptr_t TearOffStub = TearOffRefs.size() == 1 ? Finder::FunctionStart(TearOffRefs[0]) : 0;
    const int32_t TearOffIndex = TearOffStub ? Finder::VTableIndex(CdoVTable(L"/Script/Engine.ReplicationDriver"), reinterpret_cast<void*>(TearOffStub), 0x400) : -1;
    const uintptr_t TearOff = GraphVTable && TearOffIndex >= 0 ? reinterpret_cast<uintptr_t>(GraphVTable[TearOffIndex]) : 0;
    const uintptr_t TearOffEnd = TearOff && TearOff != TearOffStub && Finder::FunctionStart(TearOff) == TearOff ? Finder::FunctionEnd(TearOff) : 0;
    if (TearOffEnd <= TearOff || TearOffEnd - TearOff > 0x800)
    {
        return LeftAsIs(L"the graph's NotifyActorTearOff not found");
    }
    // lea rcx, [r64+disp8]; call FindId: the lookup in the ActorMap.
    ptrdiff_t ActorMapOffset = -1;
    for (const auto& [Site, Callee] : Finder::DirectCalls(TearOff, TearOffEnd))
    {
        const uint8_t* Lea = reinterpret_cast<const uint8_t*>(Site - 4);
        if (Callee == FindId && Site - 4 >= TearOff && (Lea[0] == 0x48 || Lea[0] == 0x49) && Lea[1] == 0x8D && (Lea[2] & 0xF8) == 0x48)
        {
            ActorMapOffset = static_cast<int8_t>(Lea[3]);
            break;
        }
    }
    // or byte ptr [r64+<the info's flags>], 2: bTearOff.
    bool bTearOffFlag = false;
    for (uintptr_t Address = TearOff; Address + 4 <= TearOffEnd && !bTearOffFlag; ++Address)
    {
        const uint8_t* Code = reinterpret_cast<const uint8_t*>(Address);
        bTearOffFlag = Code[0] == 0x80 && (Code[1] & 0xF8) == 0x48 && Code[2] == InfoFlagsOffset && Code[3] == 0x02;
    }
    if (ActorMapOffset <= 0 || ActorMapOffset != ChannelMapOffset - MapSize || !bTearOffFlag)
    {
        return LeftAsIs(L"the graph's NotifyActorTearOff isn't 6.00's or 6.20's");
    }

    Natives::UNetReplicationGraphConnection_ActorMapOffset = ActorMapOffset;
    Natives::FPerConnectionActorInfoMap_RemoveByKey = reinterpret_cast<decltype(Natives::FPerConnectionActorInfoMap_RemoveByKey)>(Remove);
    Log::Info(L"Finder: the replication graph's per-connection actor infos: ActorMap +0x{:X} (ChannelMap +0x{:X}), their remove-by-key 0x{:X} (NotifyActorChannelCleanedUp 0x{:X}, NotifyActorTearOff slot 0x{:X}, 0x{:X}; the info's flags +0x{:X})",
        ActorMapOffset, ChannelMapOffset, Remove - Finder::ImageBase(), CleanedUp - Finder::ImageBase(), TearOffIndex * 8, TearOff - Finder::ImageBase(), InfoFlagsOffset);
    return true;
}

bool Finder::ResolveBotSkydiveLive(ptrdiff_t IsSkydivingOffset)
{
    static bool bSearched = false;
    if (bSearched || Natives::AFortPlayerPawnAthena_BeginSkydiving)
    {
        return Natives::AFortPlayerPawnAthena_BeginSkydiving != nullptr;
    }
    bSearched = true;

    // AFortPlayerControllerAthena's ExitAircraft (3.0 0x9A5090) logs "ExitAircraft: %s (Pawn %s)", then, with the new
    // pawn cast to AFortPlayerPawnAthena: "mov dl, 1; mov rcx, rbx; call BeginSkydiving" (0x9A5305). BeginSkydiving
    // (0x9D7F70) starts with "cmp byte ptr [rcx + bIsSkydiving], 0": a pawn already skydiving doesn't start again.
    // 2.5's (0x9CCA20, called at 0x99A58A) has no such check: it opens with "mov byte ptr [rcx + bIsSkydiving], 1".
    // 3.5 made it virtual (bIsSkydiving moved up to AFortPlayerPawn): ExitAircraft (3.5.1 0xA19D40) does "mov rax, [rbx];
    // mov dl, 1; mov rcx, rbx; call qword ptr [rax + 0xE18]" (0xA19FD5), and AFortPlayerPawnAthena's slot holds
    // 0xA10420, which opens with the same check.
    const uintptr_t ExitAircraft = FunctionOf(L"ExitAircraft: %s (Pawn %s)");
    const uintptr_t End = ExitAircraft ? Finder::FunctionEnd(ExitAircraft) : 0;
    void** PawnVTable = nullptr;
    uintptr_t MovedTarget = 0;
    uintptr_t CastTarget = 0;
    // What each mov dl, 1 site and its callee hold, for the log when nothing is found.
    std::wstring Diagnostics;
    int32_t DiagnosedSites = 0;
    const auto HexBytes = [](uintptr_t Address, size_t Count)
    {
        std::wstring Text;
        for (size_t Index = 0; Index < Count && Finder::IsReadable(reinterpret_cast<const void*>(Address + Index), 1); ++Index)
        {
            Text += std::format(L"{}{:02X}", Index ? L" " : L"", Byte(Address + Index));
        }
        return Text;
    };
    for (uintptr_t Site = ExitAircraft ? FindInstruction(ExitAircraft, End, { 0xB2, 0x01 }) : 0; Site; Site = FindInstruction(Site + 1, End, { 0xB2, 0x01 }))
    {
        const uintptr_t Call = FindRawCall(Site + 2, std::min(End, Site + 0xA));
        uintptr_t Target = Call ? Finder::RelativeTarget(Call, 1, 5) : 0;
        if (!Target)
        {
            const uintptr_t VirtualCall = FindVirtualCall(Site + 2, std::min(End, Site + 0x10));
            if (VirtualCall && Byte(VirtualCall + 1) == 0x90)
            {
                PawnVTable = PawnVTable ? PawnVTable : CdoVTable(L"/Script/FortniteGame.FortPlayerPawnAthena");
                Target = VTableEntry(PawnVTable, VirtualSlot(VirtualCall));
            }
        }
        const auto OpensWithSkydivingCheck = [IsSkydivingOffset](uintptr_t Function)
        {
            for (uintptr_t Check = Function; Function && Check < Function + 0x20; ++Check)
            {
                const bool bCheck = IsDisp32Access(Check, 0x80, 7, IsSkydivingOffset) && Byte(Check + 6) == 0;
                const bool bSet = IsDisp32Access(Check, 0xC6, 0, IsSkydivingOffset) && (Byte(Check + 1) & 0x07) == 0x01 && Byte(Check + 6) == 1;
                if (bCheck || bSet)
                {
                    return true;
                }
            }
            return false;
        };
        // 4.2 overrides it in AFortPlayerPawnAthena (slot 0x1CB, 0xA6D480): its own bookkeeping, then Super::BeginSkydiving
        // (AFortPlayerPawn's, 0xEE15B0, at 0xA6D4B4), which opens with the check. The slot is still what ExitAircraft calls.
        // A packed build may leave either one behind jumps into readable code, so each is also read where its jumps lead.
        bool bFound = OpensWithSkydivingCheck(Target) || (Target && Finder::Unthunk(Target) != Target && OpensWithSkydivingCheck(Finder::Unthunk(Target)));
        const uintptr_t TargetEnd = !bFound && Target && Finder::FunctionStart(Target) == Target ? Finder::FunctionEnd(Target) : 0;
        for (uintptr_t SuperCall = TargetEnd ? FindRawCall(Target, std::min(TargetEnd, Target + 0x40)) : 0; SuperCall && !bFound; SuperCall = FindRawCall(SuperCall + 1, std::min(TargetEnd, Target + 0x40)))
        {
            const uintptr_t Super = Finder::RelativeTarget(SuperCall, 1, 5);
            bFound = OpensWithSkydivingCheck(Super) || (Super && OpensWithSkydivingCheck(Finder::Unthunk(Super)));
        }
        if (bFound)
        {
            Natives::AFortPlayerPawnAthena_BeginSkydiving = reinterpret_cast<decltype(Natives::AFortPlayerPawnAthena_BeginSkydiving)>(Target);
            Log::Info(L"Finder: AFortPlayerPawnAthena::BeginSkydiving at 0x{:X} (ExitAircraft 0x{:X})", Target - Finder::ImageBase(), ExitAircraft - Finder::ImageBase());
            return true;
        }
        // 4.4.2's packer moved BeginSkydiving's body: ExitAircraft (0xA80200) still does "mov dl, 1; mov rcx, rbx; call
        // BeginSkydiving" (0xA8048D, right after the AFortPlayerPawnAthena cast), but the callee (0xF48220) is only a jmp
        // into the second code section, so its skydiving check can't be read. Only that exact call shape is kept.
        const bool bCallShape = Call == Site + 5 && Byte(Site + 2) == 0x48 && Byte(Site + 3) == 0x8B && (Byte(Site + 4) & 0xF8) == 0xC8 && Target;
        if (!MovedTarget && bCallShape && Byte(Target) == 0xE9 && IsInMovedCode(Finder::RelativeTarget(Target, 1, 5)))
        {
            MovedTarget = Target;
        }
        // 5.10 CL-4225813 (ExitAircraft 0xBF1FB0) took neither path above (#368). On 5.x the call is the one right after
        // the AFortPlayerPawnAthena cast's test (CL-4240749 0xBF64FB: "test al, al; jz; mov dl, 1; mov rcx, rbx; call",
        // 5.21 0xC0941B, 5.30 0xC18C2B), so a callee whose jumps end in moved code, or that is a function of its own,
        // is taken there when nothing better was found. CL-4225813's callee (0x110D140, no .pdata) is "jmp 0x4B70E1": its
        // packer moved the body back into the first section, below the stub, so it is neither in moved code nor a function
        // start; a jmp whose hops all stay in executable code (Finder::Unthunk only follows those) is taken as well.
        // A B2 01 pair inside another instruction has no callee: 5.40's ExitAircraft (0xC21350) has one in the call at
        // 0xC21524 (E8 77 8B B2 01), before the real site (0xC21603), and reading its null target crashed the server (#386).
        const bool bAfterCast = Byte(Site - 8) == 0x84 && Byte(Site - 7) == 0xC0 && Byte(Site - 6) == 0x0F && Byte(Site - 5) == 0x84;
        const bool bJumpsToCode = Target && Byte(Target) == 0xE9 && Finder::Unthunk(Target) != Target;
        if (!CastTarget && bCallShape && bAfterCast && (IsInMovedCode(Finder::Unthunk(Target)) || Finder::FunctionStart(Target) == Target || bJumpsToCode))
        {
            CastTarget = Target;
        }
        if (DiagnosedSites++ < 4)
        {
            const uintptr_t Body = Target ? Finder::Unthunk(Target) : 0;
            Diagnostics += std::format(L"{}site 0x{:X} [{}] target 0x{:X} [{}] body 0x{:X}", Diagnostics.empty() ? L"" : L"; ", Site - Finder::ImageBase(),
                HexBytes(Site - 8, 20), Target ? Target - Finder::ImageBase() : 0, Target ? HexBytes(Target, 16) : L"", Body ? Body - Finder::ImageBase() : 0);
        }
    }
    if (MovedTarget || CastTarget)
    {
        const uintptr_t Found = MovedTarget ? MovedTarget : CastTarget;
        Natives::AFortPlayerPawnAthena_BeginSkydiving = reinterpret_cast<decltype(Natives::AFortPlayerPawnAthena_BeginSkydiving)>(Found);
        Log::Info(L"Finder: AFortPlayerPawnAthena::BeginSkydiving at 0x{:X} (ExitAircraft 0x{:X}), {}", Found - Finder::ImageBase(), ExitAircraft - Finder::ImageBase(),
            MovedTarget ? L"its body moved by the packer" : L"the call after ExitAircraft's AFortPlayerPawnAthena cast");
        return true;
    }
    Log::Error(L"Finder: AFortPlayerPawnAthena::BeginSkydiving not found (ExitAircraft 0x{:X}), player bots can't leave the aircraft ({})", ExitAircraft ? ExitAircraft - Finder::ImageBase() : 0, Diagnostics.empty() ? L"no mov dl, 1 sites" : Diagnostics);
    return false;
}

bool Finder::ResolveMovementStyleLive(ptrdiff_t CurrentMovementStyleOffset)
{
    static bool bSearched = false;
    if (bSearched || Natives::AFortPawn_SetCurrentMovementStyleIndex >= 0)
    {
        return Natives::AFortPawn_SetCurrentMovementStyleIndex >= 0;
    }
    bSearched = true;

    // AFortPawn::SetCurrentMovementStyle (21.40's name; 5.10 0x1107A30, 4.5 0xF68460, 1.11 0x8645B0) is the only
    // function that stores CurrentMovementStyle: "movzx eax, byte ptr [rdx]; mov rbx, rcx; mov [rcx + CurrentMovementStyle],
    // al", then the character movement's MaxWalkSpeed and its other speeds from GetMovementSpeed for the new style. The
    // pawn's movement style update (5.10 0x112B010) calls it through the vtable (slot 0x8C8 on 5.10).
    const auto StoresStyleWithin = [CurrentMovementStyleOffset](uintptr_t Body, uintptr_t LoadWindow)
    {
        if (!Body || !Finder::IsReadable(reinterpret_cast<const void*>(Body), LoadWindow + 0x18))
        {
            return false;
        }
        for (uintptr_t Load = Body; Load < Body + LoadWindow; ++Load)
        {
            if (Byte(Load) != 0x0F || Byte(Load + 1) != 0xB6 || Byte(Load + 2) != 0x02)
            {
                continue;
            }
            for (uintptr_t Store = Load + 3; Store < Load + 0x10; ++Store)
            {
                if (IsDisp32Access(Store, 0x88, 0, CurrentMovementStyleOffset) && (Byte(Store + 1) & 0x07) == 0x01)
                {
                    return true;
                }
            }
        }
        return false;
    };
    const auto StoresStyleItself = [&StoresStyleWithin](uintptr_t Body) { return StoresStyleWithin(Body, 0x18); };
    const auto StoresStyle = [&StoresStyleItself](uintptr_t Entry) { return StoresStyleItself(Finder::Unthunk(Entry)); };
    // 6.00's player pawn overrides it (CL-4395664 0x11DBBA0): it keeps the old style, calls AFortPawn's (0x11BAB30, the
    // store) first, then handles the change to and from style 4. The player pawn's vtable holds the override, so where no
    // slot stores the style itself, the slot whose first call does is the one.
    const auto OverrideStoresStyle = [&StoresStyleItself](uintptr_t Entry)
    {
        const uintptr_t Body = Finder::Unthunk(Entry);
        const uintptr_t SuperCall = Body && Finder::IsReadable(reinterpret_cast<const void*>(Body), 0x30) ? FindRawCall(Body, Body + 0x28) : 0;
        return SuperCall && StoresStyleItself(Finder::Unthunk(Finder::RelativeTarget(SuperCall, 1, 5)));
    };
    // 6.01's override (CL-4424678 0x11B3FD0) has AFortPawn's (0x11B3E80) inlined instead of called: it stores the style
    // itself again, behind a longer prologue that first keeps the movement component and the old style ("mov rsi,
    // [rcx+390h]; mov rdi, rcx; movzx eax, byte ptr [rdx]; movzx ebx, byte ptr [rcx+7D4h]; mov [rcx+7D4h], al"), so its
    // load is at +0x19, just past the 0x18 bytes of the first search. Looked for last, so the builds above keep their slot.
    const auto OverrideStoresStyleItself = [&StoresStyleWithin](uintptr_t Entry) { return StoresStyleWithin(Finder::Unthunk(Entry), 0x28); };
    void** PawnVTable = CurrentMovementStyleOffset > 0 ? CdoVTable(L"/Script/FortniteGame.FortPlayerPawnAthena") : nullptr;
    int32_t Index = PawnVTable ? FirstVTableIndexWhere(PawnVTable, StoresStyle) : -1;
    if (Index < 0 && PawnVTable)
    {
        Index = FirstVTableIndexWhere(PawnVTable, OverrideStoresStyle);
    }
    if (Index < 0 && PawnVTable)
    {
        Index = FirstVTableIndexWhere(PawnVTable, OverrideStoresStyleItself);
    }
    if (Index < 0)
    {
        Log::Warning(L"Finder: AFortPawn::SetCurrentMovementStyle not found (CurrentMovementStyle 0x{:X}), player bots set their movement style without its speeds",
            CurrentMovementStyleOffset);
        return false;
    }
    Natives::AFortPawn_SetCurrentMovementStyleIndex = Index;
    Log::Info(L"Finder: AFortPawn::SetCurrentMovementStyle is slot 0x{:X}", Index * 8);
    return true;
}

bool Finder::ResolveTeamChangeLive()
{
    static bool bSearched = false;
    if (bSearched || Natives::AFortGameMode_AttemptChangeTeamInfoForControllerIndex >= 0)
    {
        return Natives::AFortGameMode_AttemptChangeTeamInfoForControllerIndex >= 0;
    }
    bSearched = true;

    // AFortGameMode::PlacePlayerOnTeam (3.2 0xB6AD20, 3.1 and 3.3 the same) logs "PlacePlayerOnTeam for %s Current: %s
    // Actual: %s" between its two virtual calls on the game mode: PickTeam(CurrentTeam, Controller) (slot 0xAA0) and
    // AttemptChangeTeamInfoForController(Team, Controller) (slot 0xA88, 21.40's names), its last call. The second gets the
    // team's AFortTeamInfo (GetTeamInfo) and calls SetTeam (0xB74220): RemoveFromTeam on the old team, then
    // AFortTeamInfo::AddToTeam (0xC01AE0), which adds the controller to TeamMembers and sets its player state's team
    // (AFortPlayerStateAthena::SetPlayerTeam, 0x9FAB50: PlayerTeam, PlayerTeamPrivate and TeamIndex).
    const uintptr_t Place = FunctionOf(L"PlacePlayerOnTeam for %s Current: %s Actual: %s");
    const uintptr_t End = Place ? Finder::FunctionEnd(Place) : 0;
    std::vector<int32_t> Slots;
    for (uintptr_t Call = Place ? FindVirtualCall(Place, End) : 0; Call; Call = FindVirtualCall(Call + 6, End))
    {
        Slots.push_back(VirtualSlot(Call));
    }
    // (The team interface's GetTeam before them is a short call, [rax+28h].) In the vtable PickTeam comes right after
    // PlacePlayerOnTeam, which comes after AttemptChangeTeamInfoForController and CreateTeam: 3 slots apart.
    const int32_t Calls = static_cast<int32_t>(Slots.size());
    const int32_t Slot = Calls == 2 && Slots[1] > 0 && Slots[0] == Slots[1] + 3 ? Slots[1] : -1;
    if (Slot <= 0)
    {
        Log::Error(L"Finder: AFortGameMode::AttemptChangeTeamInfoForController not found (PlacePlayerOnTeam 0x{:X}, {} virtual calls), player bots join their team without its AFortTeamInfo",
            Place ? Place - Finder::ImageBase() : 0, Calls);
        return false;
    }
    Natives::AFortGameMode_AttemptChangeTeamInfoForControllerIndex = Slot;
    Log::Info(L"Finder: AFortGameMode::AttemptChangeTeamInfoForController is slot 0x{:X} (PlacePlayerOnTeam 0x{:X})", Slot * 8, Place - Finder::ImageBase());
    return true;
}

bool Finder::ResolveTeamRemoveLive()
{
    static bool bSearched = false;
    if (bSearched)
    {
        return Natives::AFortTeamInfo_RemoveFromTeam != nullptr;
    }
    bSearched = true;

    // AFortTeamInfo::RemoveFromTeam (4.2 0xCEDDC0) logs "Removing %s from team %s" once it has found the controller in
    // TeamMembers (AddToTeam's line is "Adding %s to team %s"); the line's only reference on 4.2. The function opens with
    // its null check of the controller ("test rdx, rdx", 4.2 0xCEDDC0), which tells it from a split-off logging block.
    const std::vector<uintptr_t> Refs = Finder::StringRefs(L"Removing %s from team %s");
    uintptr_t Remove = Refs.size() == 1 ? Finder::FunctionStart(Refs.front()) : 0;
    bool bChecksController = false;
    for (uintptr_t Check = Remove; Remove && Check < Remove + 0x30 && !bChecksController; ++Check)
    {
        bChecksController = Byte(Check) == 0x48 && Byte(Check + 1) == 0x85 && Byte(Check + 2) == 0xD2;
    }
    Remove = bChecksController ? Remove : 0;
    if (!Remove)
    {
        Log::Warning(L"Finder: AFortTeamInfo::RemoveFromTeam not found ({} references to its log line), a player bot leaving the match only leaves its team's TeamMembers",
            Refs.size());
        return false;
    }
    Natives::AFortTeamInfo_RemoveFromTeam = reinterpret_cast<decltype(Natives::AFortTeamInfo_RemoveFromTeam)>(Remove);
    Log::Info(L"Finder: AFortTeamInfo::RemoveFromTeam at 0x{:X}", Remove - Finder::ImageBase());
    return true;
}

namespace
{
    // mov reg, [base+disp32] (Opcode 8B) or mov [base+disp32], reg (89), 64-bit: REX.W (48, or 49 for a base of r8 to
    // r15) and ModRM 10 reg base, not rsp's SIB form. RegField is the ModRM reg field (0 rax, 1 rcx, 2 rdx).
    bool IsDisp32Mov(uintptr_t Site, uint8_t Opcode, uint8_t RegField, ptrdiff_t Displacement)
    {
        const uint8_t Rex = Byte(Site);
        const uint8_t ModRm = Byte(Site + 2);
        return (Rex == 0x48 || Rex == 0x49) && Byte(Site + 1) == Opcode && (ModRm & 0xF8) == (0x80 | (RegField << 3)) && (ModRm & 7) != 4
            && *reinterpret_cast<const int32_t*>(Site + 3) == static_cast<int32_t>(Displacement);
    }

    uintptr_t FindDisp32Mov(uintptr_t Begin, uintptr_t End, uint8_t Opcode, uint8_t RegField, ptrdiff_t Displacement)
    {
        for (uintptr_t Site = Begin; Begin && Site + 7 <= End; ++Site)
        {
            if (IsDisp32Mov(Site, Opcode, RegField, Displacement))
            {
                return Site;
            }
        }
        return 0;
    }

    // lea rcx or rdx (RegField 1 or 2), [rsp+disp8]: 48 8D 4C/54 24 XX. Its disp8, or -1.
    int32_t RspLeaDisplacement(uintptr_t Site, uint8_t RegField)
    {
        return Byte(Site) == 0x48 && Byte(Site + 1) == 0x8D && Byte(Site + 2) == (0x44 | (RegField << 3)) && Byte(Site + 3) == 0x24 ? Byte(Site + 4) : -1;
    }
}

bool Finder::ResolveSpecialActorReplicationLive(ptrdiff_t SpawnerSpecialActorIDOffset, ptrdiff_t CraftSpecialActorIDOffset, ptrdiff_t SpecialActorDataOffset)
{
    static bool bSearched = false;
    const auto Resolved = []
    {
        return Natives::AFortSpecialActorReplicationInfo_AddActorToReplicationList && Natives::AFortSpecialActorReplicationInfo_RemoveActorFromReplicationList
            && Natives::FSpecialActorInitData_FSpecialActorInitData && Natives::FSpecialActorInitData_Destructor;
    };
    if (bSearched)
    {
        return Resolved();
    }
    bSearched = true;
    const auto Rva = [](uintptr_t Address) { return Address ? Address - Finder::ImageBase() : 0; };

    // AFortAthenaExitCraftSpawner::RegisterSpawnerSpecialActor (5.40 0xBC4A10, 5.41 0xBC5AF0) names the spawner "%s_%s"
    // (its SpecialActorSpawnerTag, its name) into an FSpecialActorInitData on its stack, which it constructs first
    // (lea rcx, [rsp+50h]; ...; call FSpecialActorInitData::FSpecialActorInitData, 5.40 0xECE8A0), adds to the game
    // state's list (lea rdx, [rsp+50h]; mov rcx, [r14+0F28h], SpecialActorData; call AddActorToReplicationList,
    // 0xED2470), keeps the id when the index isn't -1 (cmp eax, 0FFFFFFFFh; jz; mov rax, [rsp+58h]; mov [rsi+360h],
    // rax, SpawnerSpecialActorID) and destroys (lea rcx, [rsp+50h]; call 0xBA5D80). Of the functions with that format,
    // the only one to store into SpawnerSpecialActorID.
    uintptr_t Register = 0;
    uintptr_t AddCall = 0;
    uintptr_t Store = 0;
    int32_t Registers = 0;
    for (const uintptr_t Ref : SpawnerSpecialActorIDOffset > 0 ? Finder::StringRefs(L"%s_%s") : std::vector<uintptr_t>())
    {
        const uintptr_t Function = Finder::FunctionStart(Ref);
        const uintptr_t End = Function ? Finder::FunctionEnd(Function) : 0;
        const uintptr_t Found = End ? FindDisp32Mov(Function, End, 0x89, 0, SpawnerSpecialActorIDOffset) : 0;
        const uintptr_t Call = Found ? FindInstruction(Found - 0x18, Found, { 0xE8, -1, -1, -1, -1, 0x83, 0xF8, 0xFF }) : 0;
        if (Call && Function != Register)
        {
            Register = Function;
            AddCall = Call;
            Store = Found;
            ++Registers;
        }
    }
    const uintptr_t Add = Registers == 1 ? Finder::RelativeTarget(AddCall, 1, 5) : 0;
    int32_t InitData = -1;
    for (uintptr_t Site = Add ? AddCall - 0x40 : 0; Site && Site < AddCall && InitData < 0; ++Site)
    {
        InitData = RspLeaDisplacement(Site, 2);
    }
    const bool bAddsToSpecialActorData = InitData >= 0 && FindDisp32Mov(AddCall - 0x40, AddCall, 0x8B, 1, SpecialActorDataOffset) != 0;
    uintptr_t Destructor = 0;
    for (uintptr_t Site = bAddsToSpecialActorData ? Store : 0; Site && Site < Store + 0x20 && !Destructor; ++Site)
    {
        Destructor = RspLeaDisplacement(Site, 1) == InitData && Byte(Site + 5) == 0xE8 ? Finder::RelativeTarget(Site + 5, 1, 5) : 0;
    }
    uintptr_t Constructor = 0;
    for (uintptr_t Site = Destructor ? Register : 0; Site && Site < AddCall && !Constructor; ++Site)
    {
        const uintptr_t Call = RspLeaDisplacement(Site, 1) == InitData ? FindRawCall(Site + 5, Site + 0x20) : 0;
        Constructor = Call ? Finder::RelativeTarget(Call, 1, 5) : 0;
    }

    // AFortSpecialActorReplicationInfo::RemoveActorFromReplicationList(FName UniqueID) (5.40 0xEEB5B0): AFortAthenaExitCraft's
    // departure (0xBB2610) takes the van off as it leaves, with the game state's SpecialActorData in rcx
    // (mov rcx, [rdi+0F28h]; test rcx, rcx; jz) and its CraftSpecialActorID in rdx (mov rdx, [rbx+908h]; call). The only
    // code that reads CraftSpecialActorID.
    std::vector<uintptr_t> Removes;
    for (const auto& [Begin, End] : CraftSpecialActorIDOffset > 0 && SpecialActorDataOffset > 0 ? Finder::ExecutableSections() : std::vector<std::pair<uintptr_t, uintptr_t>>())
    {
        for (uintptr_t Site = Begin; Site + 12 <= End; ++Site)
        {
            if (Byte(Site + 1) != 0x8B || Byte(Site + 7) != 0xE8 || !IsDisp32Mov(Site, 0x8B, 2, CraftSpecialActorIDOffset)
                || !FindDisp32Mov(std::max(Begin, Site - 0x18), Site, 0x8B, 1, SpecialActorDataOffset))
            {
                continue;
            }
            const uintptr_t Target = Finder::RelativeTarget(Site + 7, 1, 5);
            if (IsInText(Target) && std::find(Removes.begin(), Removes.end(), Target) == Removes.end())
            {
                Removes.push_back(Target);
            }
        }
    }
    const uintptr_t Remove = Removes.size() == 1 && Removes.front() != Add ? Removes.front() : 0;

    if (!Add || !Constructor || !Destructor || !Remove)
    {
        Log::Error(L"Finder: the special actor list's natives not found (RegisterSpawnerSpecialActor 0x{:X} of {}, AddActorToReplicationList 0x{:X}, init data at rsp+0x{:X} built 0x{:X} destroyed 0x{:X}, RemoveActorFromReplicationList 0x{:X} of {})",
            Rva(Register), Registers, Rva(Add), InitData, Rva(Constructor), Rva(Destructor), Rva(Remove), Removes.size());
        return false;
    }
    Natives::AFortSpecialActorReplicationInfo_AddActorToReplicationList = reinterpret_cast<decltype(Natives::AFortSpecialActorReplicationInfo_AddActorToReplicationList)>(Add);
    Natives::AFortSpecialActorReplicationInfo_RemoveActorFromReplicationList = reinterpret_cast<decltype(Natives::AFortSpecialActorReplicationInfo_RemoveActorFromReplicationList)>(Remove);
    Natives::FSpecialActorInitData_FSpecialActorInitData = reinterpret_cast<decltype(Natives::FSpecialActorInitData_FSpecialActorInitData)>(Constructor);
    Natives::FSpecialActorInitData_Destructor = reinterpret_cast<decltype(Natives::FSpecialActorInitData_Destructor)>(Destructor);
    Log::Info(L"Finder: AFortSpecialActorReplicationInfo::AddActorToReplicationList 0x{:X}, RemoveActorFromReplicationList 0x{:X}, FSpecialActorInitData 0x{:X} / 0x{:X} (RegisterSpawnerSpecialActor 0x{:X})",
        Rva(Add), Rva(Remove), Rva(Constructor), Rva(Destructor), Rva(Register));
    return true;
}

bool Finder::ResolveBuildingFoundationLive(void** FoundationVTable, ptrdiff_t DynamicFoundationTypeOffset)
{
    static bool bSearched = false;
    if (bSearched)
    {
        return Natives::ABuildingFoundation_SelectAndSetupMyBuildingLevel != nullptr;
    }
    bSearched = true;
    const auto Rva = [](uintptr_t Address) { return Address ? Address - Finder::ImageBase() : 0; };
    if (Natives::ABuildingSMActor_PostUpdateIndex < 0)
    {
        Finder::ResolveBuildingLootLive();
    }

    // ABuildingFoundation::PostUpdate (6.00 0xD11960) streams the plot's level in once the world manager has updated it:
    //   call ABuildingSMActor::PostUpdate
    //   test byte ptr [rbx+0BB0h], 1              ; bConditionalFoundation: the quota streams those
    //   jnz done
    //   cmp byte ptr [rbx+0BB2h], 0               ; DynamicFoundationType, Static
    //   jz select
    //   test byte ptr [rbx+0BB1h], 8              ; bTransformSet: a dynamic one waits for its transform
    //   jz done
    // select:
    //   call SelectAndSetupMyBuildingLevel        ; 0xD142E0 (this, RandomList)
    // done:
    //   or byte ptr [rbx+0BB1h], 4                ; bPostUpdateCalled
    // Each is matched by its opcode and ModRM ([reg+disp32], no SIB), whatever the register.
    const auto Disp32 = [](uintptr_t At) { return static_cast<ptrdiff_t>(*reinterpret_cast<const int32_t*>(At)); };
    const auto IsDisp32Operand = [](uintptr_t At, uint8_t Reg) { return (Byte(At) & 0xF8) == (0x80 | (Reg << 3)) && (Byte(At) & 7) != 4; };
    const uintptr_t PostUpdate = Natives::ABuildingSMActor_PostUpdateIndex >= 0 ? VTableEntry(FoundationVTable, Natives::ABuildingSMActor_PostUpdateIndex) : 0;
    const uintptr_t End = PostUpdate ? Finder::FunctionEnd(PostUpdate) : 0;
    uintptr_t TypeCheck = 0;
    for (uintptr_t At = DynamicFoundationTypeOffset > 0 ? PostUpdate : 0; At && At + 7 <= End && !TypeCheck; ++At)
    {
        TypeCheck = Byte(At) == 0x80 && IsDisp32Operand(At + 1, 7) && Disp32(At + 2) == DynamicFoundationTypeOffset && Byte(At + 6) == 0 ? At : 0;
    }
    // 6.20 (0xD66F80) adds Dynamic_StartDisabled (2), which streams once bFoundationEnabled is set, so the type is loaded
    // and compared in a register instead: movzx eax, byte ptr [rbx+0C6Ah]; test al, al; jz select; cmp al, 1; jnz
    // disabled; test byte ptr [rbx+0C69h], 8 (bTransformSet); ... select: call SelectAndSetupMyBuildingLevel (0xD69A10);
    // done: or byte ptr [rbx+0C69h], 4. The first test after the load is Dynamic_StartEnabled's bTransformSet, as before.
    for (uintptr_t At = !TypeCheck && DynamicFoundationTypeOffset > 0 ? PostUpdate : 0; At && At + 7 <= End && !TypeCheck; ++At)
    {
        TypeCheck = Byte(At) == 0x0F && Byte(At + 1) == 0xB6 && (Byte(At + 2) & 0xC0) == 0x80 && (Byte(At + 2) & 0x07) != 0x04 && Disp32(At + 3) == DynamicFoundationTypeOffset ? At : 0;
    }
    uintptr_t TransformSetTest = 0;
    for (uintptr_t At = TypeCheck ? TypeCheck + 7 : 0; At && At + 7 <= End && At < TypeCheck + 0x20 && !TransformSetTest; ++At)
    {
        TransformSetTest = Byte(At) == 0xF6 && IsDisp32Operand(At + 1, 0) ? At : 0;
    }
    const uintptr_t SelectCall = TransformSetTest ? FindRawCall(TransformSetTest + 7, End) : 0;
    uintptr_t PostUpdateCalledSet = 0;
    for (uintptr_t At = SelectCall ? SelectCall + 5 : 0; At && At + 7 <= End && !PostUpdateCalledSet; ++At)
    {
        PostUpdateCalledSet = Byte(At) == 0x80 && IsDisp32Operand(At + 1, 1) ? At : 0;
    }
    const uintptr_t Select = SelectCall ? Finder::RelativeTarget(SelectCall, 1, 5) : 0;
    // Where the build still logs it, the selection is the function with SelectAndSetupMyBuildingLevel's own message.
    const uintptr_t Logged = FunctionOf(L"ABuildingFoundation::SelectAndSetupMyBuildingLevel - Cannot get WorldManager!!");
    const uint8_t TransformSetBit = TransformSetTest ? Byte(TransformSetTest + 6) : 0;
    const uint8_t PostUpdateCalledBit = PostUpdateCalledSet ? Byte(PostUpdateCalledSet + 6) : 0;
    const auto SingleBit = [](uint8_t Bit) { return Bit && (Bit & (Bit - 1)) == 0; };
    if (!Select || (Logged && Logged != Select) || !SingleBit(TransformSetBit) || !SingleBit(PostUpdateCalledBit))
    {
        Log::Error(L"Finder: ABuildingFoundation's dynamic foundation streaming not found (PostUpdate 0x{:X} slot {}, DynamicFoundationType +0x{:X} read 0x{:X}, bTransformSet 0x{:X}, SelectAndSetupMyBuildingLevel 0x{:X} (logged 0x{:X}), bPostUpdateCalled 0x{:X})",
            Rva(PostUpdate), Natives::ABuildingSMActor_PostUpdateIndex, DynamicFoundationTypeOffset, Rva(TypeCheck), Rva(TransformSetTest), Rva(Select), Rva(Logged), Rva(PostUpdateCalledSet));
        return false;
    }

    // DefineNativeBitfield reads a uint32: its offset is the bit's byte rounded down to 4, its mask shifted to match.
    const ptrdiff_t TransformSetByte = Disp32(TransformSetTest + 2);
    const ptrdiff_t PostUpdateCalledByte = Disp32(PostUpdateCalledSet + 2);
    Natives::ABuildingFoundation_bTransformSet = TransformSetByte & ~ptrdiff_t(3);
    Natives::ABuildingFoundation_bTransformSetMask = static_cast<uint32_t>(TransformSetBit) << (8 * (TransformSetByte & 3));
    Natives::ABuildingFoundation_bPostUpdateCalled = PostUpdateCalledByte & ~ptrdiff_t(3);
    Natives::ABuildingFoundation_bPostUpdateCalledMask = static_cast<uint32_t>(PostUpdateCalledBit) << (8 * (PostUpdateCalledByte & 3));
    Natives::ABuildingFoundation_SelectAndSetupMyBuildingLevel = reinterpret_cast<decltype(Natives::ABuildingFoundation_SelectAndSetupMyBuildingLevel)>(Select);
    Log::Info(L"Finder: ABuildingFoundation::PostUpdate 0x{:X}: SelectAndSetupMyBuildingLevel 0x{:X}, bTransformSet +0x{:X} & 0x{:X}, bPostUpdateCalled +0x{:X} & 0x{:X}",
        Rva(PostUpdate), Rva(Select), TransformSetByte, TransformSetBit, PostUpdateCalledByte, PostUpdateCalledBit);
    return true;
}

std::vector<uint8_t*> Finder::ExitCraftSpawnTimerBinds(UFunction* StartExitCraftSpawnTimer, void** SpawnerVTable, void** ActorVTable, ptrdiff_t ExitCraftInfoOffset)
{
    std::vector<uint8_t*> Binds;
    const auto SharedEmpty = reinterpret_cast<uintptr_t>(Natives::SharedEmptyFunction);
    const auto SetTimer = reinterpret_cast<uintptr_t>(Natives::FTimerManager_InternalSetTimer);
    if (!SharedEmpty || !SetTimer || ExitCraftInfoOffset <= 0)
    {
        return Binds;
    }

    // StartExitCraftSpawnTimer's exec (5.40 0x13BED00) steps nothing and jumps to its body (0xBCAF80).
    const uintptr_t Exec = StartExitCraftSpawnTimer ? reinterpret_cast<uintptr_t>(StartExitCraftSpawnTimer->GetNativeFunc()) : 0;
    std::vector<uintptr_t> Candidates;
    for (uintptr_t Site = IsInText(Exec) ? Exec : 0; Site && Site < Exec + 0x30 && Candidates.empty(); ++Site)
    {
        const uintptr_t Target = (Byte(Site) == 0xE8 || Byte(Site) == 0xE9) ? Finder::RelativeTarget(Site, 1, 5) : 0;
        if (IsInText(Target) && Finder::FunctionStart(Target) == Target)
        {
            Candidates.push_back(Target);
        }
    }
    // The spawner's BeginPlay (5.40 0xBAACB0) sets the same timer, inlined: the slots the spawner overrides (from 1; 0 is
    // its destructor), up to the end of AActor's vtable.
    for (int32_t Index = 1; SpawnerVTable && ActorVTable && Index < 0x200; ++Index)
    {
        const uintptr_t Entry = VTableEntry(SpawnerVTable, Index);
        const uintptr_t ActorEntry = VTableEntry(ActorVTable, Index);
        if (!Entry || !ActorEntry)
        {
            break;
        }
        if (Entry != ActorEntry && std::find(Candidates.begin(), Candidates.end(), Entry) == Candidates.end())
        {
            Candidates.push_back(Entry);
        }
    }

    for (const uintptr_t Function : Candidates)
    {
        const uintptr_t End = Finder::FunctionEnd(Function);
        // Both read the craft info (its ExitCraftSpawnDelay, the timer's rate): mov rcx, [rcx+348h].
        bool bReadsExitCraftInfo = false;
        for (uintptr_t Load = End ? FindInstruction(Function, End, { -1, 0x8B, -1 }) : 0; Load && !bReadsExitCraftInfo; Load = FindInstruction(Load + 1, End, { -1, 0x8B, -1 }))
        {
            bReadsExitCraftInfo = (Byte(Load) == 0x48 || Byte(Load) == 0x49 || Byte(Load) == 0x4C || Byte(Load) == 0x4D) && (Byte(Load + 2) & 0xC0) == 0x80
                && (Byte(Load + 2) & 7) != 4 && *reinterpret_cast<const int32_t*>(Load + 3) == static_cast<int32_t>(ExitCraftInfoOffset);
        }
        // The delegate instance's method: lea rax, [shared empty function]; mov [reg+10h], rax (5.40 0xBAAF21, 0xBCB043),
        // then the timer is set with it (call FTimerManager::InternalSetTimer).
        for (uintptr_t Lea = bReadsExitCraftInfo ? FindInstruction(Function, End, { 0x48, 0x8D, 0x05 }) : 0; Lea; Lea = FindInstruction(Lea + 1, End, { 0x48, 0x8D, 0x05 }))
        {
            const uint8_t ModRm = Byte(Lea + 9);
            if (Finder::RelativeTarget(Lea, 3, 7) == SharedEmpty && (Byte(Lea + 7) == 0x48 || Byte(Lea + 7) == 0x49) && Byte(Lea + 8) == 0x89 && (ModRm & 0xF8) == 0x40
                && (ModRm & 7) != 4 && Byte(Lea + 10) == 0x10 && CallWithin(Lea, 0x100, SetTimer))
            {
                Binds.push_back(reinterpret_cast<uint8_t*>(Lea));
            }
        }
    }
    return Binds;
}
