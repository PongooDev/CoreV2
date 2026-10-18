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

    void Append(const TArray& Other)
    {
        Reserve(ArrayNum + Other.ArrayNum);
        for (const T& Item : Other)
        {
            Add(Item);
        }
    }

    void RemoveAt(int32_t Index)
    {
        Data[Index].~T();
        if constexpr (std::is_trivially_copyable_v<T>)
        {
            memmove(Data + Index, Data + Index + 1, sizeof(T) * (ArrayNum - Index - 1));
        }
        else
        {
            for (int32_t Move = Index; Move < ArrayNum - 1; ++Move)
            {
                new (Data + Move) T(std::move(Data[Move + 1]));
                Data[Move + 1].~T();
            }
        }
        --ArrayNum;
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
