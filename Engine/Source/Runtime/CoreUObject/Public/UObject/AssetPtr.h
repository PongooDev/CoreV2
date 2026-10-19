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
