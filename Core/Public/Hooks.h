#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace Hooks
{
    bool Detour(std::wstring_view Name, void* Target, void* Replacement, void** Original);
    bool SwapVTable(std::wstring_view Name, void** VTable, int32_t Index, void* Replacement, void** Original = nullptr);
    bool RedirectCall(std::wstring_view Name, uint8_t* CallSite, void* Destination);
    bool WriteBytes(std::wstring_view Name, void* Address, std::span<const uint8_t> Bytes);
    bool SkipBranch(std::wstring_view Name, uint8_t* Branch);
    bool EnableAll();

    template <typename T>
    bool Detour(std::wstring_view Name, void* Target, T Replacement, T* Original)
    {
        return Detour(Name, Target, reinterpret_cast<void*>(Replacement), reinterpret_cast<void**>(Original));
    }
}
