#pragma once

#include "Reflection.h"
#include "UObject/Object.h"

struct FFortAbilitySetHandle
{
    DefineUnrealStruct(FFortAbilitySetHandle, FortniteGame)
};

class UFortAbilitySet : public UObject
{
    DefineUnrealClass(UFortAbilitySet, FortniteGame)
};
