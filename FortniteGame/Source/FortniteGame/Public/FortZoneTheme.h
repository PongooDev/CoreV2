#pragma once

#include "GameplayTagContainer.h"
#include "UObject/AssetPtr.h"
#include "UObject/Object.h"

class UWorld;

class EFortZoneType
{
    DefineUnrealEnum(EFortZoneType, FortniteGame)
    DefineUEnumValue(PVE)
    DefineUEnumValue(PVP)
    DefineUEnumValue(Keep)
    DefineUEnumValue(SingleZone)
    DefineUEnumValue(Max_None)
};

class UFortZoneTheme : public UObject
{
    DefineUnrealClass(UFortZoneTheme, FortniteGame)

public:
    DefineUProperty(EFortZoneType, ZoneType)
    DefineUProperty(FGameplayTagContainer, ZoneTags)
    DefineUProperty(FGameplayTagContainer, InitialPlayerSpawnTags)
    DefineUProperty(FGameplayTagContainer, BackupPlayerSpawnTags)
    DefineUProperty(TAssetPtr<UWorld>, ZoneToUse)
    DefineUProperty(int32_t, PlaylistId)
    // The account item a Storm Shield's zone theme saves into (UFortCloudSaveItemDefinition: OutpostCore_PvE_01..04).
    DefineUProperty(TAssetPtr<UObject>, CloudSaveItemDefinition)

    // That item's asset name (OutpostCore_PvE_03), empty when the theme has none.
    FString GetCloudSaveItemName() const
    {
        return Member_CloudSaveItemDefinition.IsAvailable() ? CloudSaveItemDefinition.ToStringReference().GetAssetName() : FString();
    }
};
