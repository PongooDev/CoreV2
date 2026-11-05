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
    DefineUEnumValue(Max_None)

    // 3.5 drops Test (Max_None follows TutorialGate there; the theater's own bIsTestTheater stays): InvalidEnumValue,
    // which no theater's type matches, where it is gone.
    static EFortTheaterType GetTestIfAvailable() { return EFortTheaterType{ StaticEnumInfo().FindValue(L"Test") }; }
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
