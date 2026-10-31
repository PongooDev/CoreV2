#include "pch.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectGlobals.h"

FString FSoftObjectPath::ToString() const
{
    // Most of the time there is no sub-path, so the asset path is the whole string.
    FString AssetPathString = GetAssetPathString();
    if (!ObjectLayout::Current.bSoftObjectPaths || SubPathString().IsEmpty())
    {
        return AssetPathString;
    }
    FString FullPathString = std::move(AssetPathString);
    FullPathString += L':';
    FullPathString += SubPathString();
    return FullPathString;
}

void FSoftObjectPath::SetPath(FString Path)
{
    if (Path.IsEmpty() || Path.Equals(L"None", ESearchCase::CaseSensitive))
    {
        // Empty path, just empty the pathname.
        Reset();
    }
    else if (!ObjectLayout::Current.bSoftObjectPaths)
    {
        AssetLongPathname = std::move(Path);
    }
    else
    {
        const int32_t ColonIndex = Path.Find(L":", ESearchCase::CaseSensitive);
        if (ColonIndex >= 0)
        {
            // Has a subobject: split on that, then make a name of the path before it.
            MutableSubPathString() = Path.Mid(ColonIndex + 1);
            MutableAssetPathName() = FName(*Path.Left(ColonIndex));
        }
        else
        {
            MutableAssetPathName() = FName(*Path);
            MutableSubPathString().Reset();
        }
    }
}

UObject* FSoftObjectPath::TryLoad() const
{
    UObject* LoadedObject = nullptr;
    if (!IsNull())
    {
        LoadedObject = LoadObject<UObject>(nullptr, *ToString());
    }
    return LoadedObject;
}

UObject* FSoftObjectPath::ResolveObject() const
{
    if (IsNull())
    {
        return nullptr;
    }
    return FindObject<UObject>(nullptr, *ToString());
}
