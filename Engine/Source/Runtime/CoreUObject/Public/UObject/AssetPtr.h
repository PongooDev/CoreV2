#pragma once

#include "Misc/StringAssetReference.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/WeakObjectPtr.h"

struct FAssetPtr
{
    const FStringAssetReference& GetUniqueID() const { return ObjectID; }

    FWeakObjectPtr WeakPtr;
    int32_t TagAtLastTest = 0;
    FStringAssetReference ObjectID;
};

static_assert(sizeof(FAssetPtr) == 0x20);

template <class TClass>
class TAssetSubclassOf
{
public:
    const FStringAssetReference& ToStringReference() const { return AssetPtr.GetUniqueID(); }

    UClass* LoadSynchronous() const
    {
        if (UClass* Loaded = static_cast<UClass*>(AssetPtr.WeakPtr.Get()))
        {
            return Loaded;
        }
        const FStringAssetReference& Reference = ToStringReference();
        return Reference.IsValid() ? LoadObject<UClass>(nullptr, *Reference.ToString()) : nullptr;
    }

private:
    FAssetPtr AssetPtr;
};

template <class T>
class TAssetPtr
{
public:
    const FStringAssetReference& ToStringReference() const { return AssetPtr.GetUniqueID(); }

    T* Get() const
    {
        return static_cast<T*>(AssetPtr.WeakPtr.Get());
    }

    T* LoadSynchronous() const
    {
        if (T* Loaded = Get())
        {
            return Loaded;
        }
        const FStringAssetReference& Reference = ToStringReference();
        return Reference.IsValid() ? LoadObject<T>(nullptr, *Reference.ToString()) : nullptr;
    }

private:
    FAssetPtr AssetPtr;
};

static_assert(sizeof(TAssetPtr<UObject>) == sizeof(FAssetPtr));
