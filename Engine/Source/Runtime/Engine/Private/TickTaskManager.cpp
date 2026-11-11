#include "pch.h"
#include "Engine/EngineBaseTypes.h"
#include "Engine/Level.h"
#include "Finder.h"
#include "Log.h"
#include "Natives.h"
#include "UObject/Class.h"

#include <algorithm>
#include <cstdlib>
#include <format>
#include <new>
#include <optional>
#include <set>
#include <vector>

namespace
{
    uint8_t Byte(uintptr_t Address)
    {
        return *reinterpret_cast<const uint8_t*>(Address);
    }

    // FTickFunction's bRegistered as the build tests it: cmp byte ptr [reg+Offset], 0 (a byte, 3.2 and 5.40's +0xD; Mask
    // 0) or test byte ptr [reg+Offset], Mask (a bit, 1.8.1's 0x40 of +0xC), with a disp8 and an optional REX.B.
    struct FRegisteredFlag
    {
        int32_t Offset = -1;
        uint8_t Mask = 0;

        bool operator==(const FRegisteredFlag& Other) const = default;
    };

    // The ModRM of [reg+disp8] with Reg as its reg field, any base but rsp (a SIB byte) and rbp.
    bool IsDisp8(uint8_t ModRm, uint8_t Reg)
    {
        return (ModRm & 0xC0) == 0x40 && ((ModRm >> 3) & 0x07) == Reg && (ModRm & 0x07) != 0x04;
    }

    uintptr_t SkipRexB(uintptr_t Site)
    {
        return Byte(Site) == 0x41 ? Site + 1 : Site;
    }

    // The flag test among a function's first instructions (after its prologue's pushes and stack frame).
    std::optional<FRegisteredFlag> FlagTestAt(uintptr_t Function)
    {
        for (uintptr_t Site = Function; Site < Function + 0x14; ++Site)
        {
            const uintptr_t Op = SkipRexB(Site);
            if (Byte(Op) == 0x80 && IsDisp8(Byte(Op + 1), 7) && Byte(Op + 3) == 0x00)
            {
                return FRegisteredFlag{ Byte(Op + 2), 0 };
            }
            if (Byte(Op) == 0xF6 && IsDisp8(Byte(Op + 1), 0) && Byte(Op + 3) != 0 && (Byte(Op + 3) & (Byte(Op + 3) - 1)) == 0)
            {
                return FRegisteredFlag{ Byte(Op + 2), Byte(Op + 3) };
            }
        }
        return std::nullopt;
    }

    // Whether [Begin, End) sets (bSet) or clears the flag: mov byte ptr [reg+Offset], 1 / 0, or or / and byte ptr
    // [reg+Offset], Mask / ~Mask.
    bool WritesFlag(uintptr_t Begin, uintptr_t End, const FRegisteredFlag& Flag, bool bSet)
    {
        for (uintptr_t Site = Begin; Site + 4 <= End; ++Site)
        {
            const uintptr_t Op = SkipRexB(Site);
            if (Byte(Op + 2) != static_cast<uint8_t>(Flag.Offset))
            {
                continue;
            }
            if (Flag.Mask == 0 && Byte(Op) == 0xC6 && IsDisp8(Byte(Op + 1), 0) && Byte(Op + 3) == (bSet ? 1 : 0))
            {
                return true;
            }
            if (Flag.Mask != 0 && Byte(Op) == 0x80 && IsDisp8(Byte(Op + 1), bSet ? 1 : 4) && Byte(Op + 3) == static_cast<uint8_t>(bSet ? Flag.Mask : ~Flag.Mask))
            {
                return true;
            }
        }
        return false;
    }

    std::vector<uintptr_t> CallTargets(uintptr_t Function, uintptr_t End)
    {
        std::vector<uintptr_t> Targets;
        for (const auto& [Call, Target] : Finder::DirectCalls(Function, End))
        {
            Targets.push_back(Target);
        }
        return Targets;
    }

    std::set<uintptr_t> CallingFunctions(uintptr_t Target)
    {
        std::set<uintptr_t> Functions;
        for (const uintptr_t Site : Finder::CallsTo(Target, false))
        {
            if (const uintptr_t Function = Finder::FunctionStart(Site))
            {
                Functions.insert(Function);
            }
        }
        return Functions;
    }

    // Core's tick functions' vtable: the build's FTickFunction vtable with Core's ExecuteTick and DiagnosticMessage.
    // Four slots are copied: the build's three (destructor, ExecuteTick, DiagnosticMessage), and a later engine's
    // DiagnosticContext where there is one.
    constexpr int32_t VTableSlots = 4;
    void* CoreVTable[VTableSlots] = {};
    bool bCoreVTableMade = false;
}

bool FTickFunction::ResolveNatives()
{
    static std::optional<bool> bResolved;
    if (bResolved)
    {
        return *bResolved;
    }
    bResolved = false;

    // FTickTaskManager::Get makes the manager on first use, and its constructor registers the "dumpticks" console
    // command: 5.40 inlines the constructor into Get (0x28A8720), 3.2 and 1.8.1 call it (3.2's Get 0x23D00D0, 1.8.1's
    // 0x24C38F0). Get is one of the functions holding the string, or calls one. Every FTickFunction function that
    // reaches its level's tick functions through the manager calls Get first:
    //   UnRegisterTickFunction (5.40 0x28C8850, 3.2 0x23D9FA0, 1.8.1 0x24CDAD0): if (bRegistered) { Get();
    //     TickTaskLevel->RemoveTickFunction(this); bRegistered = false; }, two calls and nothing else;
    //   RegisterTickFunction(ULevel*) (5.40 0x28BC9B0, 3.2 0x23D6A60, 1.8.1 0x24C9530): if (!bRegistered) { Get(); ...
    //     add it to the level's TickTaskLevel; bRegistered = true; }.
    // SetTickFunctionEnable(bool) (5.40 0x28C3610, 3.2 0x23D9190) calls RemoveTickFunction and AddTickFunction on its
    // TickTaskLevel when registered, without Get. All three test bRegistered first.
    std::set<uintptr_t> Holders;
    for (const uintptr_t Ref : Finder::StringRefs(L"Dumps all tick functions registered with FTickTaskManager to log."))
    {
        if (const uintptr_t Function = Finder::FunctionStart(Ref))
        {
            Holders.insert(Function);
        }
    }
    // A holder many functions call is Get itself (5.40's, 9 callers); one called from a function or two is the
    // constructor (Get, and 3.2's tick test), or a tick test with Get inlined, whose callers are looked at instead.
    std::set<uintptr_t> Getters;
    for (const uintptr_t Holder : Holders)
    {
        const std::set<uintptr_t> Callers = CallingFunctions(Holder);
        if (Callers.size() >= 3)
        {
            Getters.insert(Holder);
        }
        else
        {
            Getters.insert(Callers.begin(), Callers.end());
        }
    }

    std::set<uintptr_t> UnRegisters;
    std::set<uintptr_t> Registers;
    uintptr_t Remove = 0;
    std::optional<FRegisteredFlag> Flag;
    bool bConsistent = true;
    for (const uintptr_t Get : Getters)
    {
        for (const uintptr_t Function : CallingFunctions(Get))
        {
            const uintptr_t End = Finder::FunctionEnd(Function);
            const std::optional<FRegisteredFlag> Tested = End ? FlagTestAt(Function) : std::nullopt;
            if (!Tested)
            {
                continue;
            }
            const std::vector<uintptr_t> Targets = CallTargets(Function, End);
            if (End - Function <= 0x40 && Targets.size() == 2 && Targets[0] == Get && WritesFlag(Function, End, *Tested, false))
            {
                bConsistent &= (!Flag || *Flag == *Tested) && (!Remove || Remove == Targets[1]);
                Flag = Tested;
                Remove = Targets[1];
                UnRegisters.insert(Function);
            }
            else if (End - Function <= 0x100 && std::find(Targets.begin(), Targets.end(), Get) != Targets.end() && WritesFlag(Function, End, *Tested, true)
                && !WritesFlag(Function, End, *Tested, false))
            {
                Registers.insert(Function);
            }
        }
    }
    // The Register candidates test the same flag.
    std::erase_if(Registers, [&](uintptr_t Function) { return !Flag || FlagTestAt(Function) != Flag; });

    std::set<uintptr_t> Enables;
    for (const uintptr_t Function : Remove ? CallingFunctions(Remove) : std::set<uintptr_t>())
    {
        const uintptr_t End = Finder::FunctionEnd(Function);
        if (!End || End - Function > 0x80 || FlagTestAt(Function) != Flag || UnRegisters.contains(Function))
        {
            continue;
        }
        const std::vector<uintptr_t> Targets = CallTargets(Function, End);
        const bool bCallsGet = std::any_of(Targets.begin(), Targets.end(), [&](uintptr_t Target) { return Getters.contains(Target); });
        const bool bCallsOther = std::any_of(Targets.begin(), Targets.end(), [&](uintptr_t Target) { return Target != Remove; });
        if (!bCallsGet && bCallsOther)
        {
            Enables.insert(Function);
        }
    }

    const auto Rva = [](uintptr_t Address) { return Address ? Address - Finder::ImageBase() : 0; };
    if (!bConsistent || UnRegisters.size() != 1 || Registers.size() != 1 || Enables.size() != 1 || !Flag)
    {
        Log::Error(L"Finder: FTickFunction's registration not found (\"dumpticks\" in {} functions, {} getters; {} UnRegisterTickFunction, {} RegisterTickFunction, {} SetTickFunctionEnable{}): Core's tick functions aren't used",
            Holders.size(), Getters.size(), UnRegisters.size(), Registers.size(), Enables.size(), bConsistent ? L"" : L", their bRegistered tests differ");
        return false;
    }
    Natives::FTickFunction_UnRegisterTickFunction = reinterpret_cast<decltype(Natives::FTickFunction_UnRegisterTickFunction)>(*UnRegisters.begin());
    Natives::FTickFunction_RegisterTickFunction = reinterpret_cast<decltype(Natives::FTickFunction_RegisterTickFunction)>(*Registers.begin());
    Natives::FTickFunction_SetTickFunctionEnable = reinterpret_cast<decltype(Natives::FTickFunction_SetTickFunctionEnable)>(*Enables.begin());
    Natives::FTickFunction_bRegistered = Flag->Offset;
    Natives::FTickFunction_bRegisteredMask = Flag->Mask;
    Log::Info(L"Finder: FTickFunction::RegisterTickFunction 0x{:X}, UnRegisterTickFunction 0x{:X}, SetTickFunctionEnable 0x{:X}, bRegistered +0x{:X}{}",
        Rva(*Registers.begin()), Rva(*UnRegisters.begin()), Rva(*Enables.begin()), Flag->Offset, Flag->Mask ? std::format(L" mask 0x{:X}", Flag->Mask) : std::wstring());
    bResolved = true;
    return true;
}

void FTickFunction::RegisterTickFunction(ULevel* Level)
{
    Natives::FTickFunction_RegisterTickFunction(this, Level);
}

void FTickFunction::UnRegisterTickFunction()
{
    Natives::FTickFunction_UnRegisterTickFunction(this);
}

void FTickFunction::SetTickFunctionEnable(bool bInEnabled)
{
    Natives::FTickFunction_SetTickFunctionEnable(this, bInEnabled);
}

bool FTickFunction::IsTickFunctionRegistered() const
{
    const uint8_t Value = reinterpret_cast<const uint8_t*>(this)[Natives::FTickFunction_bRegistered];
    return Natives::FTickFunction_bRegisteredMask ? (Value & Natives::FTickFunction_bRegisteredMask) != 0 : Value != 0;
}

FCoreTickFunction* FCoreTickFunction::OwnerOf(uint8_t* Struct)
{
    // The owner follows the struct, 8-aligned.
    const int32_t Size = FTickFunction::StaticStruct()->GetStructureSize();
    return *reinterpret_cast<FCoreTickFunction**>(Struct + ((Size + 7) & ~7));
}

void FCoreTickFunction::ExecuteTickThunk(uint8_t* This, float DeltaTime, int32_t, int32_t, const void*)
{
    OwnerOf(This)->ExecuteTick(DeltaTime);
}

FString* FCoreTickFunction::DiagnosticMessageThunk(uint8_t* This, FString* Result)
{
    // A virtual returning FString: the caller's buffer comes after this, and is returned.
    return new (Result) FString(OwnerOf(This)->DiagnosticMessage().c_str());
}

bool FCoreTickFunction::Initialize()
{
    if (Storage)
    {
        return true;
    }
    UScriptStruct* Struct = FTickFunction::ResolveNatives() ? FTickFunction::StaticStruct() : nullptr;
    const int32_t Size = Struct ? Struct->GetStructureSize() : 0;
    if (Size <= Natives::FTickFunction_bRegistered || Size < 0x10)
    {
        return false;
    }

    // The engine's constructor (TCppStructOps<FTickFunction>::Construct, 5.40 0x28933B0: vtable, tick groups, the
    // flags, Enabled, LastTickGameTimeSeconds -1), then Core's vtable over the build's.
    const int32_t OwnerOffset = (Size + 7) & ~7;
    Storage = static_cast<uint8_t*>(std::calloc(1, OwnerOffset + sizeof(FCoreTickFunction*)));
    Struct->InitializeStruct(Storage);
    void** const BuildVTable = *reinterpret_cast<void***>(Storage);
    if (!BuildVTable || !Finder::IsReadable(BuildVTable, sizeof(void*) * VTableSlots) || GetTickFunction()->IsTickFunctionRegistered())
    {
        Log::Error(L"FTickFunction: the build's struct wasn't constructed (vtable {}), Core's tick functions aren't used", static_cast<void*>(BuildVTable));
        std::free(Storage);
        Storage = nullptr;
        return false;
    }
    if (!bCoreVTableMade)
    {
        bCoreVTableMade = true;
        std::copy(BuildVTable, BuildVTable + VTableSlots, CoreVTable);
        CoreVTable[1] = reinterpret_cast<void*>(&ExecuteTickThunk);
        CoreVTable[2] = reinterpret_cast<void*>(&DiagnosticMessageThunk);
    }
    *reinterpret_cast<void***>(Storage) = CoreVTable;
    *reinterpret_cast<FCoreTickFunction**>(Storage + OwnerOffset) = this;
    return true;
}

FCoreTickFunction::~FCoreTickFunction()
{
    if (!Storage)
    {
        return;
    }
    // While the engine exits its tick task manager and levels go first (and a process ending leaves this to the OS).
    if (Natives::GIsRequestingExit && *Natives::GIsRequestingExit)
    {
        return;
    }
    // ~FTickFunction (through the struct's DestroyStruct) unregisters it as well; it is unregistered first so the
    // engine's destructor runs on a function no level holds.
    GetTickFunction()->UnRegisterTickFunction();
    FTickFunction::StaticStruct()->DestroyStruct(Storage);
    std::free(Storage);
    Storage = nullptr;
}
