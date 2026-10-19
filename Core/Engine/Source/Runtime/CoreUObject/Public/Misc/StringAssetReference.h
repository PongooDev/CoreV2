#pragma once

#include "Containers/UnrealString.h"

struct FStringAssetReference
{
    FStringAssetReference() = default;

    FStringAssetReference(const FString& PathString)
        : AssetLongPathname(PathString)
    {
    }

    const FString& ToString() const { return AssetLongPathname; }

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

    bool IsValid() const { return AssetLongPathname.Len() > 0; }
    bool IsNull() const { return AssetLongPathname.Len() == 0; }

    FString AssetLongPathname;
};
static_assert(sizeof(FStringAssetReference) == 0x10);
