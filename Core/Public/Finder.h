#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace Finder
{
    uintptr_t ImageBase();
    uintptr_t TextBegin();
    uintptr_t TextEnd();

    std::vector<uintptr_t> StringRefs(const wchar_t* Text);
    std::vector<uintptr_t> StringRefs(const char* Text);

    uintptr_t FunctionStart(uintptr_t Address);
    uintptr_t FunctionEnd(uintptr_t FunctionStart);

    uintptr_t RelativeTarget(uintptr_t Instruction, uint32_t OperandOffset, uint32_t InstructionLength);
    uintptr_t NextCall(uintptr_t From, size_t MaxDistance);
    std::vector<std::pair<uintptr_t, uintptr_t>> DirectCalls(uintptr_t Begin, uintptr_t End);
    std::vector<uintptr_t> CallsTo(uintptr_t Target, bool bIncludeJumps);

    int32_t VTableIndex(void** VTable, void* Function, int32_t MaxSlots);
    void** VTableContaining(uintptr_t Function, int32_t Slot, int32_t Slots);
    uintptr_t ImportSlot(const char* Module, const char* Symbol);
    bool IsReadable(const void* Pointer, size_t Size);

    bool ResolveRuntime();
    bool ResolveServer();
    bool ResolveProcessEvent();
    bool ResolveInitListen();
}
