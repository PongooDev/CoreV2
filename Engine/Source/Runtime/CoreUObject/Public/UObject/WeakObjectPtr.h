#pragma once

#include "Templates/TypeHash.h"

#include <cstdint>

class UObject;
struct FUObjectItem;

struct FWeakObjectPtr
{
    FWeakObjectPtr() = default;

    FWeakObjectPtr(const UObject* Object)
    {
        (*this) = Object;
    }

    void operator=(const UObject* Object);

    void Reset()
    {
        ObjectIndex = -1;
        ObjectSerialNumber = 0;
    }

    UObject* Get(bool bEvenIfPendingKill = false) const;

    bool IsValid(bool bEvenIfPendingKill = false) const
    {
        return Get(bEvenIfPendingKill) != nullptr;
    }

    bool operator==(const FWeakObjectPtr& Other) const
    {
        return (ObjectIndex == Other.ObjectIndex && ObjectSerialNumber == Other.ObjectSerialNumber) || (!IsValid() && !Other.IsValid());
    }

    uint32_t GetTypeHash() const
    {
        return static_cast<uint32_t>(ObjectIndex ^ ObjectSerialNumber);
    }

private:
    FUObjectItem* Internal_GetObjectItem() const;

    int32_t ObjectIndex = -1;
    int32_t ObjectSerialNumber = 0;
};

static_assert(sizeof(FWeakObjectPtr) == 0x8);

inline uint32_t GetTypeHash(const FWeakObjectPtr& WeakObjectPtr)
{
    return WeakObjectPtr.GetTypeHash();
}

template <class T>
struct TWeakObjectPtr : private FWeakObjectPtr
{
    TWeakObjectPtr() = default;

    TWeakObjectPtr(const T* Object)
        : FWeakObjectPtr(reinterpret_cast<const UObject*>(Object))
    {
    }

    T* Get(bool bEvenIfPendingKill = false) const
    {
        return reinterpret_cast<T*>(FWeakObjectPtr::Get(bEvenIfPendingKill));
    }

    bool IsValid(bool bEvenIfPendingKill = false) const
    {
        return FWeakObjectPtr::IsValid(bEvenIfPendingKill);
    }

    T* operator->() const
    {
        return Get();
    }

    bool operator==(const TWeakObjectPtr& Other) const
    {
        return static_cast<const FWeakObjectPtr&>(*this) == static_cast<const FWeakObjectPtr&>(Other);
    }

    friend uint32_t GetTypeHash(const TWeakObjectPtr& WeakObjectPtr)
    {
        return static_cast<const FWeakObjectPtr&>(WeakObjectPtr).GetTypeHash();
    }
};
