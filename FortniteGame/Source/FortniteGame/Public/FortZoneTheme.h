#pragma once

#include "GameplayTagContainer.h"
#include "UObject/Object.h"

enum class EFortZoneType : uint8_t
{
    PVE,
    PVP,
    Keep,
    SingleZone,
    Max_None,
};

class UFortZoneTheme : public UObject
{
    DefineUnrealClass(UFortZoneTheme, FortniteGame)

public:
    DefineUProperty(EFortZoneType, ZoneType)
    DefineUProperty(FGameplayTagContainer, ZoneTags)
};
