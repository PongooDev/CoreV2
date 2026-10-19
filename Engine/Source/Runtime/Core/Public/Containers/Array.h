#pragma once

#include "HAL/UnrealMemory.h"

#include <cstring>
#include <initializer_list>
#include <new>
#include <type_traits>
#include <utility>

template <typename T>
class TArray
{
public:
    TArray() = default;

    TArray(std::initializer_list<T> Items)
    {
        Reserve(static_cast<int32_t>(Items.size()));
        for (const T& Item : Items)
        {
            Add(Item);
        }
    }

    TArray(const TArray& Other)
    {
        Append(Other);
    }

    TArray(TArray&& Other) noexcept
        : Data(Other.Data), ArrayNum(Other.ArrayNum), ArrayMax(Other.ArrayMax)
    {
        Other.Data = nullptr;
        Other.ArrayNum = 0;
        Other.ArrayMax = 0;
    }

    ~TArray()
    {
        Empty();
    }

    TArray& operator=(const TArray& Other)
    {
        if (this != &Other)
        {
            Reset();
            Append(Other);
        }
        return *this;
    }

    TArray& operator=(TArray&& Other) noexcept
    {
        if (this != &Other)
        {
            Empty();
            Data = Other.Data;
            ArrayNum = Other.ArrayNum;
            ArrayMax = Other.ArrayMax;
            Other.Data = nullptr;
            Other.ArrayNum = 0;
            Other.ArrayMax = 0;
        }
        return *this;
    }

    T* GetData() { return Data; }
    const T* GetData() const { return Data; }
    int32_t Num() const { return ArrayNum; }
    int32_t Max() const { return ArrayMax; }
    bool IsValidIndex(int32_t Index) const { return Index >= 0 && Index < ArrayNum; }

    T& operator[](int32_t Index) { return Data[Index]; }
    const T& operator[](int32_t Index) const { return Data[Index]; }

    T* begin() { return Data; }
    T* end() { return Data + ArrayNum; }
    const T* begin() const { return Data; }
    const T* end() const { return Data + ArrayNum; }

    void Reserve(int32_t Count)
    {
        if (Count > ArrayMax)
        {
            ResizeTo(Count);
        }
    }

    int32_t Add(const T& Item)
    {
        return Emplace(Item);
    }

    int32_t Add(T&& Item)
    {
        return Emplace(std::move(Item));
    }

    template <typename... TArgs>
    int32_t Emplace(TArgs&&... Args)
    {
        if (ArrayNum == ArrayMax)
        {
            ResizeTo(ArrayMax ? ArrayMax * 2 : 4);
        }
        new (Data + ArrayNum) T(std::forward<TArgs>(Args)...);
        return ArrayNum++;
    }

    int32_t AddUninitialized(int32_t Count = 1)
    {
        const int32_t Index = ArrayNum;
        if (ArrayNum + Count > ArrayMax)
        {
            const int32_t Grown = ArrayMax ? ArrayMax * 2 : 4;
            ResizeTo(ArrayNum + Count > Grown ? ArrayNum + Count : Grown);
        }
        ArrayNum += Count;
        return Index;
    }

    int32_t AddDefaulted(int32_t Count = 1)
    {
        const int32_t Index = ArrayNum;
        Reserve(ArrayNum + Count);
        for (int32_t Added = 0; Added < Count; ++Added)
        {
            new (Data + ArrayNum) T();
            ++ArrayNum;
        }
        return Index;
    }

    int32_t AddZeroed(int32_t Count = 1)
    {
        const int32_t Index = ArrayNum;
        Reserve(ArrayNum + Count);
        memset(static_cast<void*>(Data + ArrayNum), 0, sizeof(T) * Count);
        ArrayNum += Count;
        return Index;
    }

    void InsertDefaulted(int32_t Index, int32_t Count = 1)
    {
        const int32_t OldNum = ArrayNum;
        AddDefaulted(Count);
        for (int32_t Move = OldNum - 1; Move >= Index; --Move)
        {
            std::swap(Data[Move], Data[Move + Count]);
        }
    }

    void Append(const TArray& Other)
    {
        Reserve(ArrayNum + Other.ArrayNum);
        for (const T& Item : Other)
        {
            Add(Item);
        }
    }

    bool Contains(const T& Item) const
    {
        for (const T& Element : *this)
        {
            if (Element == Item)
            {
                return true;
            }
        }
        return false;
    }

    void RemoveAt(int32_t Index, int32_t Count = 1)
    {
        if (Count <= 0)
        {
            return;
        }

        for (int32_t Removed = Index; Removed < Index + Count; ++Removed)
        {
            Data[Removed].~T();
        }

        const int32_t NumToMove = ArrayNum - Index - Count;
        if constexpr (std::is_trivially_copyable_v<T>)
        {
            memmove(Data + Index, Data + Index + Count, sizeof(T) * NumToMove);
        }
        else
        {
            for (int32_t Move = 0; Move < NumToMove; ++Move)
            {
                new (Data + Index + Move) T(std::move(Data[Index + Count + Move]));
                Data[Index + Count + Move].~T();
            }
        }
        ArrayNum -= Count;
    }

    void Reset()
    {
        for (int32_t Index = 0; Index < ArrayNum; ++Index)
        {
            Data[Index].~T();
        }
        ArrayNum = 0;
    }

    void Empty()
    {
        Reset();
        FMemory::Free(Data);
        Data = nullptr;
        ArrayMax = 0;
    }

protected:
    void ResizeTo(int32_t NewMax)
    {
        if constexpr (std::is_trivially_copyable_v<T>)
        {
            Data = static_cast<T*>(FMemory::Realloc(Data, sizeof(T) * NewMax, alignof(T)));
        }
        else
        {
            T* NewData = static_cast<T*>(FMemory::Malloc(sizeof(T) * NewMax, alignof(T)));
            for (int32_t Index = 0; Index < ArrayNum; ++Index)
            {
                new (NewData + Index) T(std::move(Data[Index]));
                Data[Index].~T();
            }
            FMemory::Free(Data);
            Data = NewData;
        }
        ArrayMax = NewMax;
    }

    T* Data = nullptr;
    int32_t ArrayNum = 0;
    int32_t ArrayMax = 0;
};

template <typename T>
void* operator new(size_t Size, TArray<T>& Array)
{
    const int32_t Index = Array.AddUninitialized(1);
    return &Array[Index];
}

template <typename T>
void operator delete(void*, TArray<T>&)
{
}
