#pragma once

#include "Containers/UnrealString.h"
#include "Reflection.h"

struct FPredictionKey
{
    DefineUnrealStruct(FPredictionKey, GameplayAbilities)
    DefineUProperty(int16_t, Current)
    DefineUProperty(int16_t, Base)

    FString ToString() const
    {
        return FString::Printf(TEXT("[%d/%d]"), Current, Base);
    }
};
