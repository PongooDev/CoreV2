#pragma once

#include "HAL/UnrealMemory.h"

#include <cstdint>
#include <cstring>

class FScriptArray
{
public:
    void* GetData() { return Data; }
    const void* GetData() const { return Data; }
    int32_t Num() const { return ArrayNum; }
    bool IsValidIndex(int32_t Index) const { return Index >= 0 && Index < ArrayNum; }

    int32_t Add(int32_t Count, int32_t NumBytesPerElement)
    {
        const int32_t OldNum = ArrayNum;
        ArrayNum += Count;
        if (ArrayNum > ArrayMax)
        {
            ArrayMax = ArrayNum;
            Data = FMemory::Realloc(Data, static_cast<size_t>(ArrayMax) * NumBytesPerElement);
        }
        return OldNum;
    }

    void Remove(int32_t Index, int32_t Count, int32_t NumBytesPerElement)
    {
        const int32_t NumToMove = ArrayNum - Index - Count;
        if (NumToMove > 0)
        {
            uint8_t* Base = static_cast<uint8_t*>(Data);
            std::memmove(Base + static_cast<size_t>(Index) * NumBytesPerElement, Base + static_cast<size_t>(Index + Count) * NumBytesPerElement, static_cast<size_t>(NumToMove) * NumBytesPerElement);
        }
        ArrayNum -= Count;
    }

    void Empty()
    {
        if (Data)
        {
            FMemory::Free(Data);
        }
        Data = nullptr;
        ArrayNum = 0;
        ArrayMax = 0;
    }

    int32_t AddZeroed(int32_t Count, int32_t NumBytesPerElement)
    {
        const int32_t Index = Add(Count, NumBytesPerElement);
        std::memset(static_cast<uint8_t*>(Data) + static_cast<size_t>(Index) * NumBytesPerElement, 0, static_cast<size_t>(Count) * NumBytesPerElement);
        return Index;
    }

private:
    void* Data = nullptr;
    int32_t ArrayNum = 0;
    int32_t ArrayMax = 0;
};

static_assert(sizeof(FScriptArray) == 0x10);
