#pragma once

#include "Containers/UnrealString.h"
#include "ObjectLayout.h"
#include "Reflection.h"
#include "UObject/NameTypes.h"

// Up to 1.9.1 (UE 4.16) an asset path is one FString. 1.10 (UE 4.19) has FSoftObjectPath in its place: the path as an
// FName, then a sub-path FString, 8 bytes longer (ObjectLayout's bSoftObjectPaths). The engine's members are read in
// either layout. Core doesn't build one of its own (it would only have room for 4.16's): SetPath writes a path into the
// engine's member in the build's layout.
struct FStringAssetReference
{
    FStringAssetReference() = default;

    FString ToString() const
    {
        if (!ObjectLayout::Current.bSoftObjectPaths)
        {
            return AssetLongPathname;
        }
        FString Path = AssetPathName().IsNone() ? FString() : AssetPathName().ToString();
        const FString& SubPath = SubPathString();
        return SubPath.Len() > 0 ? Path + L":" + SubPath : Path;
    }

    // 4.16's path, held in place.
    const FString& ToStringReference() const { return AssetLongPathname; }

    // Points this engine path at Path ("/Game/Package.Asset[:SubPath]").
    void SetPath(const FString& Path)
    {
        if (!ObjectLayout::Current.bSoftObjectPaths)
        {
            AssetLongPathname = Path;
            return;
        }
        FString AssetPath;
        FString SubPath;
        if (!Path.Split(L":", &AssetPath, &SubPath, ESearchCase::CaseSensitive, ESearchDir::FromStart))
        {
            AssetPath = Path;
        }
        MutableAssetPathName() = AssetPath.Len() > 0 ? FName(*AssetPath) : FName();
        MutableSubPathString() = SubPath;
    }

    FString GetLongPackageName() const
    {
        FString PackageName;
        ToString().Split(L".", &PackageName, nullptr, ESearchCase::CaseSensitive, ESearchDir::FromStart);
        return PackageName;
    }

    FString GetAssetName() const
    {
        FString AssetName;
        ToString().Split(L".", nullptr, &AssetName, ESearchCase::CaseSensitive, ESearchDir::FromStart);
        return AssetName;
    }

    bool IsValid() const { return ObjectLayout::Current.bSoftObjectPaths ? !AssetPathName().IsNone() : AssetLongPathname.Len() > 0; }
    bool IsNull() const { return !IsValid(); }

    FString AssetLongPathname;

private:
    const FName& AssetPathName() const { return *reinterpret_cast<const FName*>(this); }
    const FString& SubPathString() const { return *reinterpret_cast<const FString*>(reinterpret_cast<uintptr_t>(this) + sizeof(FName)); }
    FName& MutableAssetPathName() { return *reinterpret_cast<FName*>(this); }
    FString& MutableSubPathString() { return *reinterpret_cast<FString*>(reinterpret_cast<uintptr_t>(this) + sizeof(FName)); }
};
static_assert(sizeof(FStringAssetReference) == 0x10);

template <>
inline constexpr size_t Reflection::SoftPathGrowth<FStringAssetReference> = 8;
