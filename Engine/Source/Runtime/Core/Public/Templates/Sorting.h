#pragma once

#include <cstdint>
#include <utility>

template <typename T, class PREDICATE_CLASS>
struct TDereferenceWrapper
{
    const PREDICATE_CLASS& Predicate;

    explicit TDereferenceWrapper(const PREDICATE_CLASS& InPredicate)
        : Predicate(InPredicate)
    {
    }

    bool operator()(const T& A, const T& B) const
    {
        return Predicate(A, B);
    }
};

template <typename T, class PREDICATE_CLASS>
struct TDereferenceWrapper<T*, PREDICATE_CLASS>
{
    const PREDICATE_CLASS& Predicate;

    explicit TDereferenceWrapper(const PREDICATE_CLASS& InPredicate)
        : Predicate(InPredicate)
    {
    }

    bool operator()(T* A, T* B) const
    {
        return Predicate(*A, *B);
    }
};

template <class T, class PREDICATE_CLASS>
void SortInternal(T* First, const int32_t Num, const PREDICATE_CLASS& Predicate)
{
    if (Num < 2)
    {
        return;
    }

    struct FRange
    {
        T* Min;
        T* Max;
    };

    FRange Pending[32] = { { First, First + Num - 1 } };
    for (FRange* Top = Pending; Top >= Pending; --Top)
    {
        FRange Range = *Top;
        for (;;)
        {
            const ptrdiff_t Count = Range.Max - Range.Min + 1;
            if (Count <= 8)
            {
                while (Range.Max > Range.Min)
                {
                    T* Largest = Range.Min;
                    for (T* Item = Range.Min + 1; Item <= Range.Max; ++Item)
                    {
                        if (Predicate(*Largest, *Item))
                        {
                            Largest = Item;
                        }
                    }
                    std::swap(*Largest, *Range.Max);
                    --Range.Max;
                }
                break;
            }

            std::swap(Range.Min[Count / 2], Range.Min[0]);
            T* Low = Range.Min;
            T* High = Range.Max + 1;
            for (;;)
            {
                while (++Low <= Range.Max && !Predicate(*Range.Min, *Low))
                {
                }
                while (--High > Range.Min && !Predicate(*High, *Range.Min))
                {
                }
                if (Low > High)
                {
                    break;
                }
                std::swap(*Low, *High);
            }
            std::swap(*Range.Min, *High);

            if (High - 1 - Range.Min >= Range.Max - Low)
            {
                if (Range.Min + 1 < High)
                {
                    *Top++ = { Range.Min, High - 1 };
                }
                if (Range.Max > Low)
                {
                    Range.Min = Low;
                    continue;
                }
            }
            else
            {
                if (Range.Max > Low)
                {
                    *Top++ = { Low, Range.Max };
                }
                if (Range.Min + 1 < High)
                {
                    Range.Max = High - 1;
                    continue;
                }
            }
            break;
        }
    }
}

template <class T, class PREDICATE_CLASS>
void Sort(T* First, const int32_t Num, const PREDICATE_CLASS& Predicate)
{
    SortInternal(First, Num, TDereferenceWrapper<T, PREDICATE_CLASS>(Predicate));
}

template <class T, class PREDICATE_CLASS>
void Sort(T** First, const int32_t Num, const PREDICATE_CLASS& Predicate)
{
    SortInternal(First, Num, TDereferenceWrapper<T*, PREDICATE_CLASS>(Predicate));
}
