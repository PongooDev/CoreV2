#pragma once

#include "Containers/Array.h"
#include "Templates/SharedPointer.h"
#include "UObject/NameTypes.h"
#include "UObject/Object.h"

#include <cstdint>

struct FAssetDataTagMap;

// What the asset registry knows of one asset without loading it (4.16: 0x58 bytes on 1.7.2 and 1.8, the stride
// UFortCheatManager::ListWeapons walks).
struct FAssetData
{
    FName ObjectPath;
    FName PackageName;
    FName PackagePath;
    FName GroupNames;
    FName AssetName;
    FName AssetClass;
    TSharedPtr<FAssetDataTagMap, ESPMode::NotThreadSafe> TagsAndValues;
    TArray<int32_t> ChunkIDs;
    uint32_t PackageFlags = 0;

    FAssetData() = default;
    FAssetData(FAssetData&&) = default;
    FAssetData(const FAssetData&) = delete;
    FAssetData& operator=(const FAssetData&) = delete;
};
static_assert(sizeof(FAssetData) == 0x58);

class UAssetManager : public UObject
{
    DefineUnrealClass(UAssetManager, Engine)

public:
    // Every asset of a primary asset type ("Weapon", "Trap", ...; DefaultGame.ini's PrimaryAssetTypesToScan), loaded or not.
    bool GetPrimaryAssetDataList(FName PrimaryAssetType, TArray<FAssetData>& AssetDataList) const;
};
