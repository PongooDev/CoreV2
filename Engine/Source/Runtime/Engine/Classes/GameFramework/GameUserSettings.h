#pragma once

#include "UObject/Object.h"

class UGameUserSettings : public UObject
{
    DefineUnrealClass(UGameUserSettings, Engine)
    DefineUFunction(GetGameUserSettings)
    DefineUFunction(SetFrameRateLimit)
    DefineUFunction(GetFrameRateLimit)
    DefineUFunction(ApplyNonResolutionSettings)

public:
    static UGameUserSettings* GetGameUserSettings();
    void SetFrameRateLimit(float NewLimit);
    float GetFrameRateLimit();
    void ApplyNonResolutionSettings();
};
