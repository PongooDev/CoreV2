#pragma once

#include "Natives.h"
#include "UObject/Object.h"
#include "UObject/UObjectArray.h"

template <class T>
class TObjectIterator
{
public:
    explicit TObjectIterator(EObjectFlags AdditionalExclusionFlags = RF_ClassDefaultObject)
        : ExclusionFlags(AdditionalExclusionFlags)
    {
        Advance();
    }

    explicit operator bool() const
    {
        return Current != nullptr;
    }

    T* operator*() const
    {
        return Current;
    }

    T* operator->() const
    {
        return Current;
    }

    TObjectIterator& operator++()
    {
        Advance();
        return *this;
    }

private:
    void Advance()
    {
        Current = nullptr;
        const FUObjectArray* Array = Natives::GUObjectArray;
        while (Array && ++Index < Array->GetObjectArrayNum())
        {
            const FUObjectItem* Item = Array->IndexToObject(Index);
            UObject* Object = Item ? reinterpret_cast<UObject*>(Item->Object) : nullptr;
            if (Object && !Item->IsUnreachable() && !Item->IsPendingKill() && !Object->HasAnyFlags(ExclusionFlags) && Object->IsA(T::StaticClass()))
            {
                Current = static_cast<T*>(Object);
                return;
            }
        }
    }

    EObjectFlags ExclusionFlags;
    int32_t Index = -1;
    T* Current = nullptr;
};
