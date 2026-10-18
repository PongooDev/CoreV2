#pragma once

#include <cstddef>
#include <cstdint>

struct FMemory
{
    static void* Malloc(size_t Count, uint32_t Alignment = 0);
    static void* Realloc(void* Original, size_t Count, uint32_t Alignment = 0);
    static void Free(void* Original);
};
