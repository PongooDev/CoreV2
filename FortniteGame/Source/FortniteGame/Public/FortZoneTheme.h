#pragma once

#include "GameplayTagContainer.h"
#include "UObject/Object.h"

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
};
