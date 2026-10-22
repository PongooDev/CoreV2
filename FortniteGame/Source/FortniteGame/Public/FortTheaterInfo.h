#pragma once

#include "GameplayTagContainer.h"
#include "UObject/Object.h"

class EFortTheaterType
{
    DefineUnrealEnum(EFortTheaterType, FortniteGame)
    DefineUEnumValue(Standard)
    DefineUEnumValue(Elder)
    DefineUEnumValue(PvP)
    DefineUEnumValue(PvP2)
    DefineUEnumValue(Tutorial)
    DefineUEnumValue(TutorialGate)
    DefineUEnumValue(Test)
    DefineUEnumValue(Max_None)
};

struct FFortTheaterRuntimeData
{
    DefineUnrealStruct(FFortTheaterRuntimeData, FortniteGame)
    DefineUProperty(EFortTheaterType, TheaterType)
    DefineUProperty(FGameplayTagContainer, TheaterTags)
};

struct FFortTheaterMapData
{
    DefineUnrealStruct(FFortTheaterMapData, FortniteGame)
    DefineBitfieldUProperty(bIsTestTheater)
    DefineUStructProperty(FFortTheaterRuntimeData, RuntimeInfo)
};
