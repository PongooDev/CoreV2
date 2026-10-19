#pragma once

#include "UObject/Class.h"

template <class TClass>
class TSubclassOf
{
public:
    TSubclassOf() = default;

    TSubclassOf(UClass* From)
        : Class(From)
    {
    }

    template <class TClassA>
    TSubclassOf(const TSubclassOf<TClassA>& From)
        : Class(*From)
    {
    }

    UClass* operator*() const
    {
        if (!Class || !Class->IsChildOf(TClass::StaticClass()))
        {
            return nullptr;
        }
        return Class;
    }

    UClass* Get() const
    {
        return **this;
    }

    UClass* operator->() const
    {
        return **this;
    }

    operator UClass*() const
    {
        return **this;
    }

private:
    UClass* Class = nullptr;
};

static_assert(sizeof(TSubclassOf<UObject>) == sizeof(UClass*));
