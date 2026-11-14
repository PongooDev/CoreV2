#include "pch.h"
#include "Engine/AssetManager.h"
#include "Finder.h"
#include "Natives.h"
#include "ObjectLayout.h"
#include "UObject/Class.h"

#include <cstring>

namespace
{
    size_t LegacyAssetDataSize()
    {
        return ObjectLayout::Current.bSoftObjectPaths ? 0x50 : 0x58;
    }

    // The reflected FAssetData's size where it differs from the layout Core knows (6.30's 0x70), else 0.
    size_t ReflectedAssetDataSize()
    {
        static size_t Size = 0;
        static bool bLooked = false;
        if (!bLooked)
        {
            bLooked = true;
            const UScriptStruct* Struct = FAssetData::StaticStruct();
            const int32_t Reflected = Struct ? Struct->GetStructureSize() : 0;
            Size = Reflected > 0 && static_cast<size_t>(Reflected) != LegacyAssetDataSize() ? static_cast<size_t>(Reflected) : 0;
        }
        return Size;
    }
}

size_t FAssetData::GetStructureSize()
{
    const size_t Reflected = ReflectedAssetDataSize();
    return Reflected ? Reflected : LegacyAssetDataSize();
}

void FAssetData::DestroyElements(uint8_t* Data, int32_t Count)
{
    if (!Data || Count <= 0)
    {
        return;
    }
    if (ReflectedAssetDataSize())
    {
        StaticStruct()->DestroyStruct(Data, Count);
        return;
    }
    for (int32_t Index = 0; Index < Count; ++Index)
    {
        reinterpret_cast<FAssetData*>(Data + static_cast<size_t>(Index) * LegacyAssetDataSize())->DestroyInPlace();
    }
}

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
