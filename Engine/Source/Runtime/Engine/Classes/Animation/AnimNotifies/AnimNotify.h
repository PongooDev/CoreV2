#pragma once

#include "UObject/Object.h"

// A notify an animation fires at its time (Engine's UAnimNotify).
class UAnimNotify : public UObject
{
    DefineUnrealClass(UAnimNotify, Engine)
};
