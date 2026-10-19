#include "pch.h"
#include "HAL/UnrealMemory.h"
#include "Natives.h"

namespace
{
    constexpr int32_t MallocSlot = 2;
    constexpr int32_t ReallocSlot = 3;
    constexpr int32_t FreeSlot = 4;

    void* Instance()
    {
        return Natives::GMalloc ? *reinterpret_cast<void**>(Natives::GMalloc) : nullptr;
    }

    void** VTable(void* Allocator)
    {
        return *reinterpret_cast<void***>(Allocator);
    }
}

void* FMemory::Malloc(size_t Count, uint32_t Alignment)
{
    void* Allocator = Instance();
    if (!Allocator)
    {
        return nullptr;
    }
    using FMalloc = void* (*)(void*, size_t, uint32_t);
    return reinterpret_cast<FMalloc>(VTable(Allocator)[MallocSlot])(Allocator, Count, Alignment);
}

void* FMemory::Realloc(void* Original, size_t Count, uint32_t Alignment)
{
    void* Allocator = Instance();
    if (!Allocator)
    {
        return nullptr;
    }
    using FRealloc = void* (*)(void*, void*, size_t, uint32_t);
    return reinterpret_cast<FRealloc>(VTable(Allocator)[ReallocSlot])(Allocator, Original, Count, Alignment);
}

void FMemory::Free(void* Original)
{
    void* Allocator = Instance();
    if (!Original || !Allocator)
    {
        return;
    }
    using FFree = void (*)(void*, void*);
    reinterpret_cast<FFree>(VTable(Allocator)[FreeSlot])(Allocator, Original);
}
