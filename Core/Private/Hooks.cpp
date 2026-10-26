#include "pch.h"
#include "Hooks.h"
#include "Log.h"

#include <mutex>

namespace
{
    std::mutex StubMutex;
    uint8_t* StubPage = nullptr;
    size_t StubUsed = 0;
    constexpr size_t StubPageSize = 0x1000;
    constexpr size_t StubSize = 16;

    uint8_t* AllocatePageNear(void* Near)
    {
        SYSTEM_INFO SystemInfo = {};
        GetSystemInfo(&SystemInfo);

        const uintptr_t Granularity = SystemInfo.dwAllocationGranularity;
        const uintptr_t Origin = reinterpret_cast<uintptr_t>(Near) & ~(Granularity - 1);
        const uintptr_t MinAddress = reinterpret_cast<uintptr_t>(SystemInfo.lpMinimumApplicationAddress);
        const uintptr_t MaxAddress = reinterpret_cast<uintptr_t>(SystemInfo.lpMaximumApplicationAddress);
        constexpr uintptr_t Range = 0x7FF00000;

        for (uintptr_t Offset = Granularity; Offset < Range; Offset += Granularity)
        {
            for (const uintptr_t Candidate : { Origin + Offset, Origin - Offset })
            {
                if (Candidate < MinAddress || Candidate > MaxAddress)
                {
                    continue;
                }

                if (void* Page = VirtualAlloc(reinterpret_cast<void*>(Candidate), StubPageSize, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE))
                {
                    return static_cast<uint8_t*>(Page);
                }
            }
        }

        return nullptr;
    }

    uint8_t* CreateJumpStub(void* Near, void* Destination)
    {
        std::scoped_lock Lock(StubMutex);

        if (!StubPage || StubUsed + StubSize > StubPageSize)
        {
            StubPage = AllocatePageNear(Near);
            StubUsed = 0;
            if (!StubPage)
            {
                return nullptr;
            }
        }

        uint8_t* Stub = StubPage + StubUsed;
        StubUsed += StubSize;

        Stub[0] = 0xFF;
        Stub[1] = 0x25;
        *reinterpret_cast<int32_t*>(Stub + 2) = 0;
        *reinterpret_cast<void**>(Stub + 6) = Destination;
        FlushInstructionCache(GetCurrentProcess(), Stub, StubSize);
        return Stub;
    }

    bool IsRel32Reachable(const uint8_t* From, const void* To)
    {
        const int64_t Distance = reinterpret_cast<const uint8_t*>(To) - From;
        return Distance >= INT32_MIN && Distance <= INT32_MAX;
    }
}

bool Hooks::Detour(std::wstring_view Name, void* Target, void* Replacement, void** Original)
{
    if (!Target)
    {
        Log::Error(L"Hook {}: target not found", Name);
        return false;
    }

    const MH_STATUS Status = MH_CreateHook(Target, Replacement, Original);
    if (Status != MH_OK)
    {
        Log::Error(L"Hook {}: MH_CreateHook failed ({})", Name, static_cast<int32_t>(Status));
        return false;
    }

    Log::Info(L"Hook {}: detour at 0x{:X}", Name, reinterpret_cast<uintptr_t>(Target));
    return true;
}

bool Hooks::SwapVTable(std::wstring_view Name, void** VTable, int32_t Index, void* Replacement, void** Original)
{
    if (!VTable)
    {
        Log::Error(L"Hook {}: vtable not found", Name);
        return false;
    }

    void** Slot = VTable + Index;
    DWORD OldProtection = 0;
    if (!VirtualProtect(Slot, sizeof(void*), PAGE_READWRITE, &OldProtection))
    {
        Log::Error(L"Hook {}: VirtualProtect failed on vtable slot 0x{:X}", Name, Index);
        return false;
    }

    if (Original)
    {
        *Original = *Slot;
    }
    *Slot = Replacement;
    VirtualProtect(Slot, sizeof(void*), OldProtection, &OldProtection);

    Log::Info(L"Hook {}: vtable 0x{:X} slot 0x{:X}", Name, reinterpret_cast<uintptr_t>(VTable), Index);
    return true;
}

bool Hooks::RedirectCall(std::wstring_view Name, uint8_t* CallSite, void* Destination)
{
    // A rel32 call, or a rel32 tail jump (kept as a jump).
    if (!CallSite || (CallSite[0] != 0xE8 && CallSite[0] != 0xE9))
    {
        Log::Error(L"Hook {}: call site 0x{:X} is not a rel32 call or jump", Name, reinterpret_cast<uintptr_t>(CallSite));
        return false;
    }

    void* Target = Destination;
    if (!IsRel32Reachable(CallSite + 5, Target))
    {
        Target = CreateJumpStub(CallSite, Destination);
        if (!Target)
        {
            Log::Error(L"Hook {}: no memory near the call site for a jump stub", Name);
            return false;
        }
    }

    uint8_t Bytes[5] = { CallSite[0] };
    *reinterpret_cast<int32_t*>(Bytes + 1) = static_cast<int32_t>(reinterpret_cast<uint8_t*>(Target) - (CallSite + 5));
    return WriteBytes(Name, CallSite, Bytes);
}

bool Hooks::WriteBytes(std::wstring_view Name, void* Address, std::span<const uint8_t> Bytes)
{
    DWORD OldProtection = 0;
    if (!Address || !VirtualProtect(Address, Bytes.size(), PAGE_EXECUTE_READWRITE, &OldProtection))
    {
        Log::Error(L"Hook {}: cannot write 0x{:X} bytes at 0x{:X}", Name, Bytes.size(), reinterpret_cast<uintptr_t>(Address));
        return false;
    }

    memcpy(Address, Bytes.data(), Bytes.size());
    VirtualProtect(Address, Bytes.size(), OldProtection, &OldProtection);
    FlushInstructionCache(GetCurrentProcess(), Address, Bytes.size());

    Log::Info(L"Hook {}: patched 0x{:X} bytes at 0x{:X}", Name, Bytes.size(), reinterpret_cast<uintptr_t>(Address));
    return true;
}

bool Hooks::SkipBranch(std::wstring_view Name, uint8_t* Branch)
{
    if (Branch && (Branch[0] & 0xF0) == 0x70)
    {
        const uint8_t Jump[] = { 0xEB };
        return WriteBytes(Name, Branch, Jump);
    }
    if (Branch && Branch[0] == 0x0F && (Branch[1] & 0xF0) == 0x80)
    {
        const uint8_t Jump[] = { 0x90, 0xE9 };
        return WriteBytes(Name, Branch, Jump);
    }
    Log::Error(L"Hook {}: no conditional jump at 0x{:X}", Name, reinterpret_cast<uintptr_t>(Branch));
    return false;
}

bool Hooks::EnableAll()
{
    const MH_STATUS Status = MH_EnableHook(MH_ALL_HOOKS);
    if (Status != MH_OK)
    {
        Log::Error(L"Hooks: MH_EnableHook failed ({})", static_cast<int32_t>(Status));
        return false;
    }
    return true;
}
