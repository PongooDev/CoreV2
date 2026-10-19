#pragma once

#include <cstdint>
#include <cstddef>

class FMemStack
{
public:
    static FMemStack& Get();

    FMemStack() = default;
    ~FMemStack();

    FMemStack(const FMemStack&) = delete;
    FMemStack& operator=(const FMemStack&) = delete;

    uint8_t* PushBytes(size_t AllocSize, size_t Alignment);

private:
    friend class FMemMark;

    struct FTaggedMemory
    {
        FTaggedMemory* Next;
        size_t DataSize;

        uint8_t* Data()
        {
            return reinterpret_cast<uint8_t*>(this) + sizeof(FTaggedMemory);
        }
    };

    void AllocateNewChunk(size_t MinSize);
    void FreeChunks(FTaggedMemory* NewTopChunk);

    uint8_t* Top = nullptr;
    uint8_t* End = nullptr;
    FTaggedMemory* TopChunk = nullptr;
    int32_t NumMarks = 0;
};

class FMemMark
{
public:
    explicit FMemMark(FMemStack& InMem);
    ~FMemMark();

    FMemMark(const FMemMark&) = delete;
    FMemMark& operator=(const FMemMark&) = delete;

    void Pop();

private:
    FMemStack& Mem;
    uint8_t* Top;
    FMemStack::FTaggedMemory* SavedChunk;
    bool bPopped = false;
};

inline void* operator new(size_t Size, FMemStack& Mem, int32_t Count = 1, size_t Align = alignof(std::max_align_t))
{
    return Mem.PushBytes(Size * Count, Align);
}

inline void operator delete(void*, FMemStack&, int32_t, size_t)
{
}
