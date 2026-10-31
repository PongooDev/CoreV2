#pragma once

#include "Containers/Array.h"
#include "HAL/UnrealMemory.h"
#include "ObjectLayout.h"
#include "Templates/SharedPointer.h"
#include "UObject/NameTypes.h"
#include "UObject/Object.h"

#include <cstdint>
#include <memory>

struct FAssetDataTagMap;

// What the asset registry knows of one asset without loading it. Up to 1.9.1 (UE 4.16) it is 0x58 bytes, the stride
// UFortCheatManager::ListWeapons walks on 1.7.2 and 1.8, with GroupNames after PackagePath. 1.10 (UE 4.19) drops
// GroupNames: 0x50 bytes, AssetName at +0x18 (AssetRegistry.AssetData), and its item type lister (0x5814F0) walks 0x50.
// Core walked 4.16's stride over 1.10's list, read names out of other members and destroyed what wasn't there, which
// crashed "cheat dance" (CoreV2 #165). Core only reads the engine's, in the build's layout (ObjectLayout's
// bSoftObjectPaths marks 4.19), from AssetName on.
struct FAssetData
{
    FName ObjectPath;
    FName PackageName;
    FName PackagePath;

    using FTagsAndValues = TSharedPtr<FAssetDataTagMap, ESPMode::NotThreadSafe>;
    FName& GetAssetName() const { return Member<FName>(0x18); }
    __declspec(property(get = GetAssetName)) FName AssetName;
    FName& GetAssetClass() const { return Member<FName>(0x20); }
    __declspec(property(get = GetAssetClass)) FName AssetClass;
    FTagsAndValues& GetTagsAndValues() const { return Member<FTagsAndValues>(0x28); }
    __declspec(property(get = GetTagsAndValues)) FTagsAndValues TagsAndValues;
    TArray<int32_t>& GetChunkIDs() const { return Member<TArray<int32_t>>(0x38); }
    __declspec(property(get = GetChunkIDs)) TArray<int32_t> ChunkIDs;
    uint32_t& GetPackageFlags() const { return Member<uint32_t>(0x48); }
    __declspec(property(get = GetPackageFlags)) uint32_t PackageFlags;

    // The build's size: 4.16's 0x58 or 4.19's 0x50.
    static size_t GetStructureSize() { return ObjectLayout::Current.bSoftObjectPaths ? 0x50 : 0x58; }

    FAssetData() = delete;
    FAssetData(const FAssetData&) = delete;
    FAssetData& operator=(const FAssetData&) = delete;

    // The engine's members that own something, given back in place (the list's elements are the engine's copies).
    void DestroyInPlace()
    {
        std::destroy_at(&GetTagsAndValues());
        std::destroy_at(&GetChunkIDs());
    }

private:
    // A member at its 4.19 offset, 8 bytes later past 4.16's GroupNames.
    template <typename T>
    T& Member(ptrdiff_t Offset) const
    {
        return *reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(this) + Offset + (ObjectLayout::Current.bSoftObjectPaths ? 0 : sizeof(FName)));
    }
};

// The engine's TArray<FAssetData>, filled by GetPrimaryAssetDataList: its elements are the build's FAssetData size, so
// they are reached and destroyed at that stride.
template <>
class TArray<FAssetData>
{
public:
    TArray() = default;
    TArray(const TArray&) = delete;
    TArray& operator=(const TArray&) = delete;

    ~TArray()
    {
        Empty();
    }

    int32_t Num() const { return ArrayNum; }

    FAssetData& operator[](int32_t Index) const
    {
        return *reinterpret_cast<FAssetData*>(Data + static_cast<size_t>(Index) * FAssetData::GetStructureSize());
    }

    void Empty()
    {
        for (int32_t Index = 0; Index < ArrayNum; ++Index)
        {
            (*this)[Index].DestroyInPlace();
        }
        FMemory::Free(Data);
        Data = nullptr;
        ArrayNum = 0;
        ArrayMax = 0;
    }

private:
    uint8_t* Data = nullptr;
    int32_t ArrayNum = 0;
    int32_t ArrayMax = 0;
};
static_assert(sizeof(TArray<FAssetData>) == 0x10);

class UAssetManager : public UObject
{
    DefineUnrealClass(UAssetManager, Engine)

public:
    // Every asset of a primary asset type ("Weapon", "Trap", ...; DefaultGame.ini's PrimaryAssetTypesToScan), loaded or not.
    bool GetPrimaryAssetDataList(FName PrimaryAssetType, TArray<FAssetData>& AssetDataList) const;
};
