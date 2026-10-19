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

    int32_t Remove(const KeyType& InKey)
    {
        return Pairs.Remove(InKey);
    }

    typename ElementSetType::TIterator begin() { return Pairs.begin(); }
    typename ElementSetType::TIterator end() { return Pairs.end(); }
    typename ElementSetType::TConstIterator begin() const { return Pairs.begin(); }
    typename ElementSetType::TConstIterator end() const { return Pairs.end(); }

private:
    ElementSetType Pairs;
};

static_assert(sizeof(TMap<int32_t, int32_t>) == 0x50);
