#pragma once

#include "UObject/Object.h"

class UFortFastLoadConfig : public UObject
{
    DefineUnrealClass(UFortFastLoadConfig, FortniteGame)

public:
    DefineBitfieldUProperty(bDisableStartingMissions)
};
