#pragma once

#include "Containers/Set.h"

template <typename KeyType, typename ValueType>
struct TPair
{
    KeyType Key;
    ValueType Value;
};

template <typename KeyType, typename ValueType>
struct TDefaultMapKeyFuncs
{
    using KeyInitType = const KeyType&;
    static constexpr bool bAllowDuplicateKeys = false;

    static KeyInitType GetSetKey(const TPair<KeyType, ValueType>& Element)
    {
        return Element.Key;
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

template <typename KeyType, typename ValueType>
class TMap
{
public:
    using ElementType = TPair<KeyType, ValueType>;
    using ElementSetType = TSet<ElementType, TDefaultMapKeyFuncs<KeyType, ValueType>>;

    int32_t Num() const
    {
        return Pairs.Num();
    }

    ValueType* Find(const KeyType& Key)
    {
        ElementType* Pair = Pairs.Find(Key);
        return Pair ? &Pair->Value : nullptr;
    }

    const ValueType* Find(const KeyType& Key) const
    {
        const ElementType* Pair = Pairs.Find(Key);
        return Pair ? &Pair->Value : nullptr;
    }

    ValueType FindRef(const KeyType& Key) const
    {
        const ElementType* Pair = Pairs.Find(Key);
        return Pair ? Pair->Value : ValueType();
    }

    ValueType& FindChecked(const KeyType& Key)
    {
        return Pairs.Find(Key)->Value;
    }

    const ValueType& FindChecked(const KeyType& Key) const
    {
        return Pairs.Find(Key)->Value;
    }

    bool Contains(const KeyType& Key) const
    {
        return Pairs.Contains(Key);
    }

    ValueType& Add(const KeyType& InKey, const ValueType& InValue)
    {
        const FSetElementId PairId = Pairs.Emplace(ElementType{ InKey, InValue });
        return Pairs[PairId].Value;
    }

    ValueType& FindOrAdd(const KeyType& Key)
    {
        if (ElementType* Pair = Pairs.Find(Key))
        {
            return Pair->Value;
        }
        return Add(Key, ValueType());
    }

    int32_t Remove(const KeyType& InKey)
    {
        return Pairs.Remove(InKey);
    }

    void Empty()
    {
        Pairs.Empty();
    }

    template <typename PREDICATE_CLASS>
    void KeySort(const PREDICATE_CLASS& Predicate)
    {
        Pairs.Sort([&Predicate](const ElementType& A, const ElementType& B) { return Predicate(A.Key, B.Key); });
    }

    template <typename PREDICATE_CLASS>
    void ValueSort(const PREDICATE_CLASS& Predicate)
    {
        Pairs.Sort([&Predicate](const ElementType& A, const ElementType& B) { return Predicate(A.Value, B.Value); });
    }

    template <bool bConst>
    class TBaseIterator
    {
        using PairIteratorType = std::conditional_t<bConst, typename ElementSetType::TConstIterator, typename ElementSetType::TIterator>;
        using ItElementType = std::conditional_t<bConst, const ElementType, ElementType>;
        using ItValueType = std::conditional_t<bConst, const ValueType, ValueType>;

    public:
        explicit TBaseIterator(const PairIteratorType& InPairIt)
            : PairIt(InPairIt)
        {
        }

        TBaseIterator& operator++()
        {
            ++PairIt;
            return *this;
        }

        explicit operator bool() const
        {
            return static_cast<bool>(PairIt);
        }

        const KeyType& Key() const
        {
            return PairIt->Key;
        }

        ItValueType& Value() const
        {
            return PairIt->Value;
        }

        ItElementType& operator*() const
        {
            return *PairIt;
        }

        ItElementType* operator->() const
        {
            return &*PairIt;
        }

    private:
        PairIteratorType PairIt;
    };

    using TIterator = TBaseIterator<false>;
    using TConstIterator = TBaseIterator<true>;

    TIterator CreateIterator() { return TIterator(Pairs.CreateIterator()); }
    TConstIterator CreateConstIterator() const { return TConstIterator(Pairs.CreateConstIterator()); }

    typename ElementSetType::TIterator begin() { return Pairs.begin(); }
    typename ElementSetType::TIterator end() { return Pairs.end(); }
    typename ElementSetType::TConstIterator begin() const { return Pairs.begin(); }
    typename ElementSetType::TConstIterator end() const { return Pairs.end(); }

private:
    ElementSetType Pairs;
};

static_assert(sizeof(TMap<int32_t, int32_t>) == 0x50);
