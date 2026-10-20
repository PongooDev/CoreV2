#pragma once

#include "Containers/ScriptArray.h"
#include "UObject/Class.h"

template <class T>
class TStructArray : public FScriptArray
{
public:
    static int32_t ElementSize() { return T::StaticStruct()->GetStructureSize(); }

    T& operator[](int32_t Index) { return *reinterpret_cast<T*>(static_cast<uint8_t*>(GetData()) + static_cast<size_t>(Index) * ElementSize()); }
    const T& operator[](int32_t Index) const { return *reinterpret_cast<const T*>(static_cast<const uint8_t*>(GetData()) + static_cast<size_t>(Index) * ElementSize()); }

    T& AddDefaulted()
    {
        const int32_t Index = Add(1, ElementSize());
        T& Element = (*this)[Index];
        T::StaticStruct()->InitializeStruct(&Element);
        return Element;
    }

    void RemoveAt(int32_t Index)
    {
        T::StaticStruct()->DestroyStruct(&(*this)[Index]);
        Remove(Index, 1, ElementSize());
    }
};

static_assert(sizeof(TStructArray<FScriptArray>) == sizeof(FScriptArray));
