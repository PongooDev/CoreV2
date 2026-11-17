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
    TAssetPtr() = default;

    const FStringAssetReference& ToStringReference() const { return AssetPtr.GetUniqueID(); }

    // Points this engine member at Path, unloaded, in the build's layout (FStringAssetReference::SetPath).
    void SetPath(const FString& Path)
    {
        AssetPtr.WeakPtr.Reset();
        AssetPtr.TagAtLastTest = 0;
        AssetPtr.ObjectID.SetPath(Path);
    }

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

// FSoftObjectPtr by value, in UE 4.19+'s layout (ObjectLayout bSoftObjectPaths, 0x28 bytes): what Core hands a native
// that takes a TSoftObjectPtr by value. MSVC passes such an argument as a pointer to the caller's copy, and the callee
// destroys it, so the copy's SubPathString is freed by the game (Core's FString allocates with the game's GMalloc).
struct FSoftObjectPtr
{
    struct FSoftObjectPath
    {
        FName AssetPathName;
        FString SubPathString;
    };

    FSoftObjectPtr() = default;

    // A copy of an engine TAssetPtr's object and path. Only for bSoftObjectPaths builds: Asset's path is read in 4.19's
    // layout.
    template <class T>
    explicit FSoftObjectPtr(const TAssetPtr<T>& Asset)
        : WeakPtr(reinterpret_cast<const UObject*>(Asset.Get())),
          ObjectID(*reinterpret_cast<const FSoftObjectPath*>(&Asset.ToStringReference()))
    {
    }

    FWeakObjectPtr WeakPtr;
    int32_t TagAtLastTest = 0;
    FSoftObjectPath ObjectID;
};

static_assert(sizeof(FSoftObjectPtr) == 0x28 && offsetof(FSoftObjectPtr, ObjectID) == 0x10);

// FSoftObjectPtr (1.10) holds an FSoftObjectPath, 8 bytes longer, where FAssetPtr holds an FStringAssetReference.
template <class T>
inline constexpr size_t Reflection::SoftPathGrowth<TAssetPtr<T>> = 8;

template <class T>
inline constexpr size_t Reflection::SoftPathGrowth<TAssetSubclassOf<T>> = 8;

// The Index-th element of an engine array of soft pointers or paths, whose elements are SoftPathGrowth bytes longer
// than Core's on 1.10.
template <typename TElement>
TElement& SoftElementAt(const TArray<TElement>& Array, int32_t Index)
{
    const size_t Stride = sizeof(TElement) + (ObjectLayout::Current.bSoftObjectPaths ? Reflection::SoftPathGrowth<TElement> : 0);
    return *reinterpret_cast<TElement*>(reinterpret_cast<uintptr_t>(Array.GetData()) + Index * Stride);
}
