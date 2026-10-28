#include "pch.h"
#include "Engine/AssetManager.h"
#include "Finder.h"
#include "Natives.h"
#include "ObjectLayout.h"

#include <cstring>

bool UAssetManager::GetPrimaryAssetDataList(FName PrimaryAssetType, TArray<FAssetData>& AssetDataList) const
{
    if (Natives::UAssetManager_GetPrimaryAssetDataListIndex < 0 && !Finder::ResolveAssetManagerLive())
    {
        return false;
    }

    // FPrimaryAssetType is an FName passed by value: one register.
    uint64_t Type = 0;
    std::memcpy(&Type, &PrimaryAssetType, sizeof(Type));
    return CallVirtual<bool>(this, Natives::UAssetManager_GetPrimaryAssetDataListIndex, Type, &AssetDataList);
}
