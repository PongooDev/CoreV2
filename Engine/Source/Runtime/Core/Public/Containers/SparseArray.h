#pragma once

#include "Containers/Array.h"
#include "Containers/BitArray.h"

#include <new>
#include <type_traits>

struct FSparseArrayAllocationInfo
{
    int32_t Index;
    void* Pointer;
};

inline void* operator new(size_t, const FSparseArrayAllocationInfo& Allocation)
{
    return Allocation.Pointer;
}

inline void operator delete(void*, const FSparseArrayAllocationInfo&)
{
}

template <typename ElementType>
class TSparseArray
{
    struct alignas(ElementType) FElementStorage
    {
        uint8_t Bytes[sizeof(ElementType)];
    };

    union FElementOrFreeListLink
    {
        FElementStorage ElementData;
        struct
        {
            int32_t PrevFreeIndex;
            int32_t NextFreeIndex;
        };
    };

public:
    TSparseArray() = default;

    ~TSparseArray()
    {
        Empty();
    }

    TSparseArray(const TSparseArray&) = delete;
    TSparseArray& operator=(const TSparseArray&) = delete;

    int32_t Num() const
    {
        return Data.Num() - NumFreeIndices;
    }

    int32_t GetMaxIndex() const
    {
        return Data.Num();
    }

    bool IsAllocated(int32_t Index) const
    {
        return AllocationFlags[Index];
    }

    ElementType& operator[](int32_t Index)
    {
        return *reinterpret_cast<ElementType*>(&Data[Index].ElementData);
    }

    const ElementType& operator[](int32_t Index) const
    {
        return *reinterpret_cast<const ElementType*>(&Data[Index].ElementData);
    }

    FSparseArrayAllocationInfo AllocateIndex(int32_t Index)
    {
        AllocationFlags.SetBit(Index, true);
        return { Index, &Data[Index].ElementData };
    }

    FSparseArrayAllocationInfo AddUninitialized()
    {
        int32_t Index;
        if (NumFreeIndices)
        {
            Index = FirstFreeIndex;
            FirstFreeIndex = Data[FirstFreeIndex].NextFreeIndex;
            --NumFreeIndices;
            if (NumFreeIndices)
            {
                Data[FirstFreeIndex].PrevFreeIndex = -1;
            }
        }
        else
        {
            Index = Data.AddUninitialized(1);
            AllocationFlags.Add(false);
        }
        return AllocateIndex(Index);
    }

    void RemoveAt(int32_t Index)
    {
        if constexpr (!std::is_trivially_destructible_v<ElementType>)
        {
            (*this)[Index].~ElementType();
        }
        RemoveAtUninitialized(Index);
    }

    void RemoveAtUninitialized(int32_t Index)
    {
        if (NumFreeIndices)
        {
            Data[FirstFreeIndex].PrevFreeIndex = Index;
        }
        FElementOrFreeListLink& IndexData = Data[Index];
        IndexData.PrevFreeIndex = -1;
        IndexData.NextFreeIndex = NumFreeIndices > 0 ? FirstFreeIndex : -1;
        FirstFreeIndex = Index;
        ++NumFreeIndices;
        AllocationFlags.SetBit(Index, false);
    }

    void Empty()
    {
        if constexpr (!std::is_trivially_destructible_v<ElementType>)
        {
            for (int32_t Index = 0; Index < Data.Num(); ++Index)
            {
                if (IsAllocated(Index))
                {
                    (*this)[Index].~ElementType();
                }
            }
        }
        Data.Empty();
        FirstFreeIndex = -1;
        NumFreeIndices = 0;
        AllocationFlags.Empty();
    }

    template <bool bConst>
    class TBaseIterator
    {
        using ArrayType = std::conditional_t<bConst, const TSparseArray, TSparseArray>;
        using ItElementType = std::conditional_t<bConst, const ElementType, ElementType>;

    public:
        TBaseIterator(ArrayType& InArray, int32_t InIndex)
            : Array(InArray), Index(InIndex)
        {
            SkipUnallocated();
        }

        TBaseIterator& operator++()
        {
            ++Index;
            SkipUnallocated();
            return *this;
        }

        explicit operator bool() const
        {
            return Index < Array.GetMaxIndex();
        }

        int32_t GetIndex() const
        {
            return Index;
        }

        ItElementType& operator*() const
        {
            return Array[Index];
        }

        ItElementType* operator->() const
        {
            return &Array[Index];
        }

        bool operator!=(const TBaseIterator& Other) const
        {
            return Index != Other.Index;
        }

    private:
        void SkipUnallocated()
        {
            while (Index < Array.GetMaxIndex() && !Array.IsAllocated(Index))
            {
                ++Index;
            }
        }

        ArrayType& Array;
        int32_t Index;
    };

    using TIterator = TBaseIterator<false>;
    using TConstIterator = TBaseIterator<true>;

    TIterator begin() { return TIterator(*this, 0); }
    TIterator end() { return TIterator(*this, GetMaxIndex()); }
    TConstIterator begin() const { return TConstIterator(*this, 0); }
    TConstIterator end() const { return TConstIterator(*this, GetMaxIndex()); }

private:
    TArray<FElementOrFreeListLink> Data;
    TBitArray AllocationFlags;
    int32_t FirstFreeIndex = -1;
    int32_t NumFreeIndices = 0;
};

static_assert(sizeof(TSparseArray<int32_t>) == 0x38);
