#include "pch.h"
#include "HAL/UnrealMemory.h"
#include "Natives.h"

void* FMemory::Malloc(size_t Count, uint32_t Alignment)
{
    return Natives::FMemory_Malloc(Count, Alignment);
}

void* FMemory::Realloc(void* Original, size_t Count, uint32_t Alignment)
{
    return Natives::FMemory_Realloc(Original, Count, Alignment);
}

void FMemory::Free(void* Original)
{
    if (Original)
    {
        Natives::FMemory_Free(Original);
    }
}
