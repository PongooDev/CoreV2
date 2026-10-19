#include "pch.h"
#include "Misc/MemStack.h"
#include "HAL/UnrealMemory.h"

namespace
{
    constexpr size_t MinChunkSize = 64 * 1024;
}

FMemStack& FMemStack::Get()
{
    thread_local FMemStack* Stack = new FMemStack();
    return *Stack;
}

FMemStack::~FMemStack()
{
    FreeChunks(nullptr);
}

uint8_t* FMemStack::PushBytes(size_t AllocSize, size_t Alignment)
{
    uint8_t* Result = reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(Top) + Alignment - 1) & ~(Alignment - 1));
    if (!Top || Result + AllocSize > End)
    {
        AllocateNewChunk(AllocSize + Alignment);
        Result = reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(Top) + Alignment - 1) & ~(Alignment - 1));
    }
    Top = Result + AllocSize;
    return Result;
}

void FMemStack::AllocateNewChunk(size_t MinSize)
{
    const size_t DataSize = MinSize > MinChunkSize ? MinSize : MinChunkSize;
    FTaggedMemory* Chunk = static_cast<FTaggedMemory*>(FMemory::Malloc(sizeof(FTaggedMemory) + DataSize));
    Chunk->Next = TopChunk;
    Chunk->DataSize = DataSize;
    TopChunk = Chunk;
    Top = Chunk->Data();
    End = Top + DataSize;
}

void FMemStack::FreeChunks(FTaggedMemory* NewTopChunk)
{
    while (TopChunk != NewTopChunk)
    {
        FTaggedMemory* RemoveChunk = TopChunk;
        TopChunk = TopChunk->Next;
        FMemory::Free(RemoveChunk);
    }
    Top = TopChunk ? TopChunk->Data() : nullptr;
    End = TopChunk ? Top + TopChunk->DataSize : nullptr;
}

FMemMark::FMemMark(FMemStack& InMem)
    : Mem(InMem), Top(InMem.Top), SavedChunk(InMem.TopChunk)
{
    ++Mem.NumMarks;
}

FMemMark::~FMemMark()
{
    Pop();
}

void FMemMark::Pop()
{
    if (bPopped)
    {
        return;
    }
    bPopped = true;
    --Mem.NumMarks;

    if (SavedChunk != Mem.TopChunk)
    {
        Mem.FreeChunks(SavedChunk);
    }
    Mem.Top = Top;
    if (!Top)
    {
        Mem.End = nullptr;
    }
}
