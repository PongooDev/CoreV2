#pragma once

#include "Containers/UnrealString.h"
#include "ObjectLayout.h"
#include "Reflection.h"
#include "UObject/NameTypes.h"

class UObject;

// A reference to an object by its path, "/Game/Package.Asset[:SubPath]". 1.1-1.9.1 (UE 4.16) hold the whole path in
// one FString (FStringAssetReference's AssetLongPathname). 1.10 (UE 4.19) holds UE 4.18's layout: the asset path as an
// FName, then the sub-path, 8 bytes longer (ObjectLayout's bSoftObjectPaths). Core reads and writes the engine's paths
// in place, in the build's layout, and never builds or copies one: it only has room for 4.16's.
struct FSoftObjectPath
{
    FSoftObjectPath() = delete;
    FSoftObjectPath(const FSoftObjectPath&) = delete;
    FSoftObjectPath& operator=(const FSoftObjectPath&) = delete;

    // The path, "/Game/Package.Asset[:SubPath]".
    FString ToString() const;

    // The asset path, package and asset, without the sub-path. 4.16 doesn't split them: its whole path.
    FString GetAssetPathString() const
    {
        if (!ObjectLayout::Current.bSoftObjectPaths)
        {
            return AssetLongPathname;
        }
        return AssetPathName().IsNone() ? FString() : AssetPathName().ToString();
    }

    // "/Game/Package", without the asset name and sub-path.
    FString GetLongPackageName() const
    {
        FString PackageName;
        GetAssetPathString().Split(L".", &PackageName, nullptr, ESearchCase::CaseSensitive, ESearchDir::FromStart);
        return PackageName;
    }

    // The asset name, without "/Game/Package." and the sub-path.
    FString GetAssetName() const
    {
        FString AssetName;
        GetAssetPathString().Split(L".", nullptr, &AssetName, ESearchCase::CaseSensitive, ESearchDir::FromStart);
        return AssetName;
    }

    // Points this path at Path, "/Game/Package.Asset[:SubPath]"; an empty one resets it.
    void SetPath(FString Path);

    // Loads the object, which can be slow. Null for a null path.
    UObject* TryLoad() const;

    // The object if it's loaded.
    UObject* ResolveObject() const;

    // Points this path at nothing.
    void Reset()
    {
        if (!ObjectLayout::Current.bSoftObjectPaths)
        {
            AssetLongPathname.Reset();
            return;
        }
        MutableAssetPathName() = FName();
        MutableSubPathString().Reset();
    }

    bool IsValid() const
    {
        return ObjectLayout::Current.bSoftObjectPaths ? !AssetPathName().IsNone() : AssetLongPathname.Len() > 0;
    }

    bool IsNull() const { return !IsValid(); }

    // 4.16's member. 1.10's path isn't one FString: read it with the methods above.
    FString AssetLongPathname;

private:
    // UE 4.18's members, in AssetLongPathname's place and the 8 bytes past it.
    const FName& AssetPathName() const { return *reinterpret_cast<const FName*>(this); }
    const FString& SubPathString() const { return *reinterpret_cast<const FString*>(reinterpret_cast<uintptr_t>(this) + sizeof(FName)); }
    FName& MutableAssetPathName() { return *reinterpret_cast<FName*>(this); }
    FString& MutableSubPathString() { return *reinterpret_cast<FString*>(reinterpret_cast<uintptr_t>(this) + sizeof(FName)); }
};
static_assert(sizeof(FSoftObjectPath) == 0x10);

// A path to a class.
struct FSoftClassPath : public FSoftObjectPath
{
    FSoftClassPath() = delete;
};
static_assert(sizeof(FSoftClassPath) == 0x10);

// 1.1-1.9.1's (UE 4.16) names, which UE 4.18 renamed.
typedef FSoftObjectPath FStringAssetReference;
typedef FSoftClassPath FStringClassReference;

template <>
inline constexpr size_t Reflection::SoftPathGrowth<FSoftObjectPath> = 8;

template <>
inline constexpr size_t Reflection::SoftPathGrowth<FSoftClassPath> = 8;

// The Index-th element of an engine array of soft paths or pointers, or structs holding one, whose elements are
// SoftPathGrowth bytes longer than Core's on 1.10.
template <typename TElement>
TElement& SoftElementAt(const TArray<TElement>& Array, int32_t Index)
{
    const size_t Stride = sizeof(TElement) + (ObjectLayout::Current.bSoftObjectPaths ? Reflection::SoftPathGrowth<TElement> : 0);
    return *reinterpret_cast<TElement*>(reinterpret_cast<uintptr_t>(Array.GetData()) + Index * Stride);
}
