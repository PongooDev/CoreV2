#pragma once

#include "GameplayTagContainer.h"
#include "UObject/Object.h"

enum class EFortTheaterType : uint8_t
{
    Standard,
    Elder,
    PvP,
    PvP2,
    Tutorial,
    TutorialGate,
    Test,
    Max_None,
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
