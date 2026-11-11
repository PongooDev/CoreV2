#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace Hooks
{
    bool Detour(std::wstring_view Name, void* Target, void* Replacement, void** Original);
    bool SwapVTable(std::wstring_view Name, void** VTable, int32_t Index, void* Replacement, void** Original = nullptr);
    bool RedirectCall(std::wstring_view Name, uint8_t* CallSite, void* Destination);
    /// <summary>
    /// The redirect for a stripped function the game binds rather than calls: a `lea r64, [rip+rel32]` that takes its
    /// address (a timer's or an event's delegate stores it as the method it calls) takes Destination's instead, through a
    /// jump stub near the site when Destination is out of rel32 reach.
    /// </summary>
    bool RedirectAddress(std::wstring_view Name, uint8_t* LeaSite, void* Destination);
    bool WriteBytes(std::wstring_view Name, void* Address, std::span<const uint8_t> Bytes);
    bool SkipBranch(std::wstring_view Name, uint8_t* Branch);
    bool EnableAll();

    /// <summary>
    /// Holds every change to code from here on: WriteBytes (and so RedirectCall and SkipBranch) records its bytes
    /// instead of writing them, and EnableAll only notes that the detours are wanted. Vtable swaps and other data
    /// writes still go straight in. For a packed build that checks its code on the engine's first ticks (3.5+), whose
    /// server makes its changes to data until then and its changes to code after (Packed::RunBeforeEngineStart).
    /// </summary>
    void HoldCode();

    /// <summary>Writes the held bytes in the order they came and enables the detours, if EnableAll asked for them;
    /// code changes go straight in again after. False when a held write or the enable failed.</summary>
    bool ReleaseCode();

    template <typename T>
    bool Detour(std::wstring_view Name, void* Target, T Replacement, T* Original)
    {
        return Detour(Name, Target, reinterpret_cast<void*>(Replacement), reinterpret_cast<void**>(Original));
    }
}
