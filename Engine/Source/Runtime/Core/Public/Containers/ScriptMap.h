#pragma once

#include <cstdint>

// Epic's FScriptMapLayout (Containers/Map.h): where a pair's value starts and how far apart the set's elements are,
// for a TMap whose key and value types only reflection knows.
struct FScriptMapLayout
{
    int32_t ValueOffset = 0;
    int32_t ElementSize = 0;
};

// Epic's FScriptMap: a TMap seen without its types, as UMapProperty's FScriptMapHelper reads one. Its TSet keeps each
// TPair<Key, Value> in a TSetElement (the pair, then HashNextId and HashIndex) in a sparse array: the elements, then the
// allocation bits (four inline words, else a heap block), the free list and the hash. Read-only: Core only walks maps
// the game filled, so it never allocates or frees their memory.
class FScriptMap
{
public:
    static FScriptMapLayout GetScriptLayout(int32_t KeySize, int32_t KeyAlignment, int32_t ValueSize, int32_t ValueAlignment)
    {
        const auto Align = [](int32_t Value, int32_t Alignment) { return (Value + Alignment - 1) & ~(Alignment - 1); };
        const int32_t PairAlignment = KeyAlignment > ValueAlignment ? KeyAlignment : ValueAlignment;
        FScriptMapLayout Layout;
        Layout.ValueOffset = Align(KeySize, ValueAlignment);
        const int32_t PairSize = Align(Layout.ValueOffset + ValueSize, PairAlignment);
        // TSetElement: the pair, FSetElementId HashNextId, int32 HashIndex; a free slot's two int32 links fit in it.
        const int32_t ElementAlignment = PairAlignment > 4 ? PairAlignment : 4;
        Layout.ElementSize = Align(PairSize + 8, ElementAlignment);
        return Layout;
    }

    int32_t GetMaxIndex() const
    {
        return ElementsNum;
    }

    bool IsValidIndex(int32_t Index) const
    {
        const uint32_t* Bits = AllocationFlagsSecondaryData ? AllocationFlagsSecondaryData : AllocationFlagsInlineData;
        return Index >= 0 && Index < ElementsNum && Index < AllocationFlagsNumBits && (Bits[Index / 32] & (1u << (Index & 31))) != 0;
    }

    int32_t Num() const
    {
        return ElementsNum - NumFreeIndices;
    }

    // The pair at Index: its key here, its value at Layout.ValueOffset.
    uint8_t* GetData(int32_t Index, const FScriptMapLayout& Layout) const
    {
        return ElementsData + static_cast<size_t>(Index) * Layout.ElementSize;
    }

private:
    uint8_t* ElementsData;
    int32_t ElementsNum;
    int32_t ElementsMax;
    uint32_t AllocationFlagsInlineData[4];
    uint32_t* AllocationFlagsSecondaryData;
    int32_t AllocationFlagsNumBits;
    int32_t AllocationFlagsMaxBits;
    int32_t FirstFreeIndex;
    int32_t NumFreeIndices;
    void* HashInlineData;
    void* HashSecondaryData;
    int32_t HashSize;
};

static_assert(sizeof(FScriptMap) == 0x50);
