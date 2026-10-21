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

class UAbilitySystemComponent;

struct FScopedPredictionWindow
{
    FScopedPredictionWindow(UAbilitySystemComponent* AbilitySystemComponent, const FPredictionKey& InPredictionKey, bool InSetReplicatedPredictionKey = true);
    ~FScopedPredictionWindow();

    FScopedPredictionWindow(const FScopedPredictionWindow&) = delete;
    FScopedPredictionWindow& operator=(const FScopedPredictionWindow&) = delete;

private:
    alignas(16) uint8_t Storage[0x80] = {};
    bool bConstructed = false;
};
