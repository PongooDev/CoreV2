#pragma once

#include "Templates/Casts.h"
#include "UObject/PersistentObjectPtr.h"
#include "UObject/SoftObjectPath.h"

// A soft pointer: an object's path, and a weak pointer to it once it's loaded. 8 bytes longer on 1.10, by its path.
struct FSoftObjectPtr : public TPersistentObjectPtr<FSoftObjectPath>
{
    // The object, loaded if it isn't.
    UObject* LoadSynchronous() const
    {
        UObject* Asset = Get();
        if (Asset == nullptr && IsPending())
        {
            // The engine's resolves the pointer again once TryLoad has loaded it, which sets its weak pointer: Core
            // leaves that to the engine and takes the object TryLoad returns.
            Asset = ToSoftObjectPath().TryLoad();
        }
        return Asset;
    }

    const FSoftObjectPath& ToSoftObjectPath() const { return GetUniqueID(); }
    FString ToString() const { return ToSoftObjectPath().ToString(); }
    FString GetLongPackageName() const { return ToSoftObjectPath().GetLongPackageName(); }
    FString GetAssetName() const { return ToSoftObjectPath().GetAssetName(); }
};
static_assert(sizeof(FSoftObjectPtr) == 0x20);

// A soft pointer to a T.
template <class T = UObject>
struct TSoftObjectPtr
{
    // Points this engine member at Path ("/Game/Package.Asset[:SubPath]"), not loaded. The engine assigns an
    // FSoftObjectPath, which Core can't build: it writes the path into the member's own.
    void SetPath(FString Path)
    {
        SoftObjectPtr.ResetWeakPtr();
        SoftObjectPtr.GetUniqueID().SetPath(std::move(Path));
    }

    // The object, or null if it isn't loaded or isn't a T.
    T* Get() const { return Cast<T>(SoftObjectPtr.Get()); }

    bool IsValid() const { return Get() != nullptr; }
    bool IsPending() const { return SoftObjectPtr.IsPending(); }
    bool IsNull() const { return SoftObjectPtr.IsNull(); }

    const FSoftObjectPath& ToSoftObjectPath() const { return SoftObjectPtr.GetUniqueID(); }
    FString ToString() const { return ToSoftObjectPath().ToString(); }
    FString GetLongPackageName() const { return ToSoftObjectPath().GetLongPackageName(); }
    FString GetAssetName() const { return ToSoftObjectPath().GetAssetName(); }

    // The object, loaded if it isn't; null if it isn't a T.
    T* LoadSynchronous() const
    {
        UObject* Asset = SoftObjectPtr.LoadSynchronous();
        return Cast<T>(Asset);
    }

private:
    FSoftObjectPtr SoftObjectPtr;
};
static_assert(sizeof(TSoftObjectPtr<UObject>) == sizeof(FSoftObjectPtr));

// A soft pointer to a class that is, or derives from, TClass.
template <class TClass = UObject>
class TSoftClassPtr
{
public:
    // The class, or null if it isn't loaded or isn't a TClass.
    UClass* Get() const
    {
        UClass* Class = Cast<UClass>(SoftObjectPtr.Get());
        if (!Class || !Class->IsChildOf(TClass::StaticClass()))
        {
            return nullptr;
        }
        return Class;
    }

    bool IsValid() const { return Get() != nullptr; }
    bool IsPending() const { return SoftObjectPtr.IsPending(); }
    bool IsNull() const { return SoftObjectPtr.IsNull(); }

    const FSoftObjectPath& ToSoftObjectPath() const { return SoftObjectPtr.GetUniqueID(); }
    FString ToString() const { return ToSoftObjectPath().ToString(); }
    FString GetLongPackageName() const { return ToSoftObjectPath().GetLongPackageName(); }
    FString GetAssetName() const { return ToSoftObjectPath().GetAssetName(); }

    // The class, loaded if it isn't; null if it isn't a TClass.
    UClass* LoadSynchronous() const
    {
        UObject* Asset = SoftObjectPtr.LoadSynchronous();
        UClass* Class = Cast<UClass>(Asset);
        if (!Class || !Class->IsChildOf(TClass::StaticClass()))
        {
            return nullptr;
        }
        return Class;
    }

private:
    FSoftObjectPtr SoftObjectPtr;
};
static_assert(sizeof(TSoftClassPtr<UObject>) == sizeof(FSoftObjectPtr));

// 1.1-1.9.1's (UE 4.16) names, which UE 4.18 renamed.
typedef FSoftObjectPtr FAssetPtr;
template <class T = UObject>
using TAssetPtr = TSoftObjectPtr<T>;
template <class TClass = UObject>
using TAssetSubclassOf = TSoftClassPtr<TClass>;

template <>
inline constexpr size_t Reflection::SoftPathGrowth<FSoftObjectPtr> = 8;

template <class T>
inline constexpr size_t Reflection::SoftPathGrowth<TSoftObjectPtr<T>> = 8;

template <class TClass>
inline constexpr size_t Reflection::SoftPathGrowth<TSoftClassPtr<TClass>> = 8;
