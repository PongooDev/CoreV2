#pragma once

#include "Containers/SparseArray.h"
#include "Templates/Sorting.h"
#include "HAL/UnrealMemory.h"
#include "Templates/TypeHash.h"

#include <bit>
#include <cstring>
#include <utility>

class FSetElementId
{
public:
    FSetElementId() = default;

    explicit FSetElementId(int32_t InIndex)
        : Index(InIndex)
    {
    }

    bool IsValidId() const
    {
        return Index != -1;
    }

    int32_t AsInteger() const
    {
        return Index;
    }

    operator int32_t() const
    {
        return Index;
    }

    bool operator==(const FSetElementId& Other) const = default;

private:
    int32_t Index = -1;
};

template <typename InElementType>
class TSetElement
{
public:
    template <typename... ArgTypes>
    explicit TSetElement(ArgTypes&&... Args)
        : Value(std::forward<ArgTypes>(Args)...)
    {
    }

    InElementType Value;
    mutable FSetElementId HashNextId;
    mutable int32_t HashIndex = 0;
};

template <typename ElementType, bool bInAllowDuplicateKeys = false>
struct DefaultKeyFuncs
{
    using KeyInitType = const ElementType&;
    static constexpr bool bAllowDuplicateKeys = bInAllowDuplicateKeys;

    static KeyInitType GetSetKey(const ElementType& Element)
    {
        return Element;
    }

    static bool Matches(KeyInitType A, KeyInitType B)
    {
        return A == B;
    }

    static uint32_t GetKeyHash(KeyInitType Key)
    {
        return GetTypeHash(Key);
    }
};

template <typename InElementType, typename KeyFuncs = DefaultKeyFuncs<InElementType>>
class TSet
{
    using SetElementType = TSetElement<InElementType>;
    using KeyInitType = typename KeyFuncs::KeyInitType;

public:
    TSet() = default;

    ~TSet()
    {
        if (Hash.SecondaryData)
        {
            FMemory::Free(Hash.SecondaryData);
        }
    }

    TSet(const TSet&) = delete;
    TSet& operator=(const TSet&) = delete;

    int32_t Num() const
    {
        return Elements.Num();
    }

    InElementType& operator[](FSetElementId Id)
    {
        return Elements[Id].Value;
    }

    const InElementType& operator[](FSetElementId Id) const
    {
        return Elements[Id].Value;
    }

    FSetElementId FindId(KeyInitType Key) const
    {
        if (Elements.Num())
        {
            for (FSetElementId ElementId = GetTypedHash(KeyFuncs::GetKeyHash(Key)); ElementId.IsValidId(); ElementId = Elements[ElementId].HashNextId)
            {
                if (KeyFuncs::Matches(KeyFuncs::GetSetKey(Elements[ElementId].Value), Key))
                {
                    return ElementId;
                }
            }
        }
        return FSetElementId();
    }

    InElementType* Find(KeyInitType Key)
    {
        const FSetElementId ElementId = FindId(Key);
        return ElementId.IsValidId() ? &Elements[ElementId].Value : nullptr;
    }

    const InElementType* Find(KeyInitType Key) const
    {
        const FSetElementId ElementId = FindId(Key);
        return ElementId.IsValidId() ? &Elements[ElementId].Value : nullptr;
    }

    bool Contains(KeyInitType Key) const
    {
        return FindId(Key).IsValidId();
    }

    template <typename ArgType>
    FSetElementId Emplace(ArgType&& Arg, bool* bIsAlreadyInSetPtr = nullptr)
    {
        const FSparseArrayAllocationInfo ElementAllocation = Elements.AddUninitialized();
        SetElementType& Element = *new (ElementAllocation) SetElementType(std::forward<ArgType>(Arg));
        FSetElementId ElementId(ElementAllocation.Index);

        bool bIsAlreadyInSet = false;
        if (!KeyFuncs::bAllowDuplicateKeys && Elements.Num() != 1)
        {
            const FSetElementId ExistingId = FindId(KeyFuncs::GetSetKey(Element.Value));
            bIsAlreadyInSet = ExistingId.IsValidId();
            if (bIsAlreadyInSet)
            {
                Elements[ExistingId].Value.~InElementType();
                memcpy(static_cast<void*>(&Elements[ExistingId].Value), &Element.Value, sizeof(InElementType));
                Elements.RemoveAtUninitialized(ElementId);
                ElementId = ExistingId;
            }
        }

        if (!bIsAlreadyInSet && !ConditionalRehash(Elements.Num()))
        {
            HashElement(ElementId, Elements[ElementId]);
        }

        if (bIsAlreadyInSetPtr)
        {
            *bIsAlreadyInSetPtr = bIsAlreadyInSet;
        }
        return ElementId;
    }

    FSetElementId Add(const InElementType& InElement, bool* bIsAlreadyInSetPtr = nullptr)
    {
        return Emplace(InElement, bIsAlreadyInSetPtr);
    }

    void Remove(FSetElementId ElementId)
    {
        if (Elements.Num())
        {
            const SetElementType& ElementBeingRemoved = Elements[ElementId];
            for (FSetElementId* NextElementId = &GetTypedHash(ElementBeingRemoved.HashIndex); NextElementId->IsValidId(); NextElementId = &Elements[*NextElementId].HashNextId)
            {
                if (*NextElementId == ElementId)
                {
                    *NextElementId = ElementBeingRemoved.HashNextId;
                    break;
                }
            }
        }
        Elements.RemoveAt(ElementId);
    }

    int32_t Remove(KeyInitType Key)
    {
        int32_t NumRemovedElements = 0;
        if (Elements.Num())
        {
            FSetElementId* NextElementId = &GetTypedHash(KeyFuncs::GetKeyHash(Key));
            while (NextElementId->IsValidId())
            {
                SetElementType& Element = Elements[*NextElementId];
                if (KeyFuncs::Matches(KeyFuncs::GetSetKey(Element.Value), Key))
                {
                    const FSetElementId RemovedId = *NextElementId;
                    *NextElementId = Element.HashNextId;
                    Elements.RemoveAt(RemovedId);
                    ++NumRemovedElements;
                    if (!KeyFuncs::bAllowDuplicateKeys)
                    {
                        break;
                    }
                }
                else
                {
                    NextElementId = &Element.HashNextId;
                }
            }
        }
        return NumRemovedElements;
    }

    template <bool bConst>
    class TBaseIterator
    {
        using ElementIteratorType = typename TSparseArray<SetElementType>::template TBaseIterator<bConst>;
        using SparseArrayType = std::conditional_t<bConst, const TSparseArray<SetElementType>, TSparseArray<SetElementType>>;
        using ItElementType = std::conditional_t<bConst, const InElementType, InElementType>;

    public:
        TBaseIterator(SparseArrayType& InElements, int32_t InIndex)
            : ElementIt(InElements, InIndex)
        {
        }

        TBaseIterator& operator++()
        {
            ++ElementIt;
            return *this;
        }

        explicit operator bool() const
        {
            return static_cast<bool>(ElementIt);
        }

        FSetElementId GetId() const
        {
            return FSetElementId(ElementIt.GetIndex());
        }

        ItElementType& operator*() const
        {
            return ElementIt->Value;
        }

        ItElementType* operator->() const
        {
            return &ElementIt->Value;
        }

        bool operator!=(const TBaseIterator& Other) const
        {
            return ElementIt != Other.ElementIt;
        }

    private:
        ElementIteratorType ElementIt;
    };

    using TIterator = TBaseIterator<false>;
    using TConstIterator = TBaseIterator<true>;

    void Empty()
    {
        Elements.Empty();
        if (Hash.SecondaryData)
        {
            FMemory::Free(Hash.SecondaryData);
            Hash.SecondaryData = nullptr;
        }
        HashSize = 0;
    }

    template <typename PREDICATE_CLASS>
    void Sort(const PREDICATE_CLASS& Predicate)
    {
        if (Elements.Num() == 0)
        {
            return;
        }

        TArray<InElementType> Sorted;
        Sorted.Reserve(Elements.Num());
        for (const InElementType& Element : *this)
        {
            Sorted.Add(Element);
        }
        ::Sort(Sorted.GetData(), Sorted.Num(), Predicate);

        Empty();
        for (const InElementType& Element : Sorted)
        {
            Add(Element);
        }
    }

    TIterator CreateIterator() { return TIterator(Elements, 0); }
    TConstIterator CreateConstIterator() const { return TConstIterator(Elements, 0); }

    TIterator begin() { return TIterator(Elements, 0); }
    TIterator end() { return TIterator(Elements, Elements.GetMaxIndex()); }
    TConstIterator begin() const { return TConstIterator(Elements, 0); }
    TConstIterator end() const { return TConstIterator(Elements, Elements.GetMaxIndex()); }

private:
    static int32_t GetNumberOfHashBuckets(uint32_t NumHashedElements)
    {
        if (NumHashedElements >= 4)
        {
            return static_cast<int32_t>(std::bit_ceil(NumHashedElements / 2 + 8));
        }
        return 1;
    }

    FSetElementId* HashData() const
    {
        return Hash.SecondaryData ? Hash.SecondaryData : const_cast<FSetElementId*>(Hash.InlineData);
    }

    FSetElementId& GetTypedHash(int32_t HashIndex) const
    {
        return HashData()[HashIndex & (HashSize - 1)];
    }

    void HashElement(FSetElementId ElementId, const SetElementType& Element) const
    {
        Element.HashIndex = KeyFuncs::GetKeyHash(KeyFuncs::GetSetKey(Element.Value)) & (HashSize - 1);
        Element.HashNextId = GetTypedHash(Element.HashIndex);
        GetTypedHash(Element.HashIndex) = ElementId;
    }

    bool ConditionalRehash(int32_t NumHashedElements) const
    {
        const int32_t DesiredHashSize = GetNumberOfHashBuckets(NumHashedElements);
        if (NumHashedElements > 0 && (!HashSize || HashSize < DesiredHashSize))
        {
            HashSize = DesiredHashSize;
            Rehash();
            return true;
        }
        return false;
    }

    void Rehash() const
    {
        if (Hash.SecondaryData)
        {
            FMemory::Free(Hash.SecondaryData);
            Hash.SecondaryData = nullptr;
        }

        if (HashSize)
        {
            if (HashSize > 1)
            {
                Hash.SecondaryData = static_cast<FSetElementId*>(FMemory::Malloc(HashSize * sizeof(FSetElementId)));
            }
            for (int32_t HashIndex = 0; HashIndex < HashSize; ++HashIndex)
            {
                GetTypedHash(HashIndex) = FSetElementId();
            }
            for (int32_t Index = 0; Index < Elements.GetMaxIndex(); ++Index)
            {
                if (Elements.IsAllocated(Index))
                {
                    HashElement(FSetElementId(Index), Elements[Index]);
                }
            }
        }
    }

    struct alignas(8) FHashAllocator
    {
        FSetElementId InlineData[1];
        FSetElementId* SecondaryData = nullptr;
    };

    TSparseArray<SetElementType> Elements;
    mutable FHashAllocator Hash;
    mutable int32_t HashSize = 0;

    friend struct FSetLayout;
};

struct FSetLayout
{
    static_assert(sizeof(TSet<int32_t>) == 0x50);
    static_assert(offsetof(TSet<int32_t>, Hash) == 0x38);
    static_assert(offsetof(TSet<int32_t>, HashSize) == 0x48);
};
