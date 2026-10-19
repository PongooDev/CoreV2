#pragma once

#include "HAL/UnrealMemory.h"

#include <cstdint>
#include <cstring>

constexpr int32_t NumBitsPerDWORD = 32;

class TBitArray
{
public:
    TBitArray() = default;

    ~TBitArray()
    {
        if (SecondaryData)
        {
            FMemory::Free(SecondaryData);
        }
    }

    TBitArray(const TBitArray&) = delete;
    TBitArray& operator=(const TBitArray&) = delete;

    int32_t Num() const
    {
        return NumBits;
    }

    uint32_t* GetData()
    {
        return SecondaryData ? SecondaryData : InlineData;
    }

    const uint32_t* GetData() const
    {
        return SecondaryData ? SecondaryData : InlineData;
    }

    bool operator[](int32_t Index) const
    {
        return (GetData()[Index / NumBitsPerDWORD] & (1u << (Index & (NumBitsPerDWORD - 1)))) != 0;
    }

    void SetBit(int32_t Index, bool bValue)
    {
        uint32_t& Word = GetData()[Index / NumBitsPerDWORD];
        const uint32_t Mask = 1u << (Index & (NumBitsPerDWORD - 1));
        Word = bValue ? (Word | Mask) : (Word & ~Mask);
    }

    int32_t Add(bool bValue)
    {
        const int32_t Index = NumBits;
        const int32_t PreviousNumBits = NumBits;
        ++NumBits;
        if (NumBits > MaxBits)
        {
            Realloc(PreviousNumBits);
        }
        SetBit(Index, bValue);
        return Index;
    }

    void Empty()
    {
        NumBits = 0;
        if (SecondaryData)
        {
            FMemory::Free(SecondaryData);
            SecondaryData = nullptr;
        }
        MaxBits = NumInlineDWORDs * NumBitsPerDWORD;
    }

private:
    static constexpr int32_t NumInlineDWORDs = 4;

    static int32_t DivideAndRoundUp(int32_t Dividend, int32_t Divisor)
    {
        return (Dividend + Divisor - 1) / Divisor;
    }

    void Realloc(int32_t PreviousNumBits)
    {
        const int32_t PreviousNumDWORDs = DivideAndRoundUp(PreviousNumBits, NumBitsPerDWORD);
        const int32_t NumDWORDs = DivideAndRoundUp(NumBits, NumBitsPerDWORD);
        const int32_t MaxDWORDs = NumDWORDs <= NumInlineDWORDs ? NumInlineDWORDs : NumDWORDs + 3 * NumDWORDs / 8 + 16;
        MaxBits = MaxDWORDs * NumBitsPerDWORD;

        if (MaxDWORDs <= NumInlineDWORDs)
        {
            if (SecondaryData)
            {
                memcpy(InlineData, SecondaryData, PreviousNumDWORDs * sizeof(uint32_t));
                FMemory::Free(SecondaryData);
                SecondaryData = nullptr;
            }
        }
        else if (!SecondaryData)
        {
            SecondaryData = static_cast<uint32_t*>(FMemory::Malloc(MaxDWORDs * sizeof(uint32_t)));
            memcpy(SecondaryData, InlineData, PreviousNumDWORDs * sizeof(uint32_t));
        }
        else
        {
            SecondaryData = static_cast<uint32_t*>(FMemory::Realloc(SecondaryData, MaxDWORDs * sizeof(uint32_t)));
        }

        memset(GetData() + PreviousNumDWORDs, 0, (MaxDWORDs - PreviousNumDWORDs) * sizeof(uint32_t));
    }

    uint32_t InlineData[NumInlineDWORDs] = {};
    uint32_t* SecondaryData = nullptr;
    int32_t NumBits = 0;
    int32_t MaxBits = NumInlineDWORDs * NumBitsPerDWORD;
};

static_assert(sizeof(TBitArray) == 0x20);
