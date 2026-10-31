#pragma once

#include "UObject/WeakObjectPtr.h"

#include <cstdint>

class UObject;

// A weak pointer to an object, which its unique ID (TObjectID) finds again once it's gone or not yet loaded.
template <class TObjectID>
struct TPersistentObjectPtr
{
    // Resets the weak pointer only, for when the ID changes.
    void ResetWeakPtr()
    {
        WeakPtr.Reset();
        TagAtLastTest = 0;
    }

    const TObjectID& GetUniqueID() const { return ObjectID; }
    TObjectID& GetUniqueID() { return ObjectID; }

    // The object, or null if it isn't loaded. The engine keeps the object its ID finds in the weak pointer, and looks
    // again when its tag (TObjectID::GetCurrentTag) moves on: Core can't read the tag, so it looks whenever the weak
    // pointer is null, and leaves the engine's weak pointer and tag alone.
    UObject* Get() const
    {
        UObject* Object = WeakPtr.Get();
        if (!Object && ObjectID.IsValid())
        {
            Object = ObjectID.ResolveObject();
        }
        return Object;
    }

    // Whether the ID points at an object that isn't loaded.
    bool IsPending() const { return Get() == nullptr && ObjectID.IsValid(); }

    bool IsValid() const { return Get() != nullptr; }
    bool IsNull() const { return !ObjectID.IsValid(); }

private:
    mutable FWeakObjectPtr WeakPtr;
    mutable int32_t TagAtLastTest;
    TObjectID ObjectID;
};
