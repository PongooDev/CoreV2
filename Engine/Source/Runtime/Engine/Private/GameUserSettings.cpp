#include "pch.h"
#include "GameFramework/GameUserSettings.h"
#include "ReflectionParams.h"

UGameUserSettings* UGameUserSettings::GetGameUserSettings()
{
    Reflection::FParams Params(StaticFunction_GetGameUserSettings());
    return Params.Call(StaticClass()->GetDefaultObject()) ? Params.Take<UGameUserSettings*>(L"ReturnValue") : nullptr;
}

void UGameUserSettings::SetFrameRateLimit(float NewLimit)
{
    Reflection::FParams Params(StaticFunction_SetFrameRateLimit());
    Params.Set(L"NewLimit", NewLimit);
    Params.Call(this);
}

float UGameUserSettings::GetFrameRateLimit()
{
    Reflection::FParams Params(StaticFunction_GetFrameRateLimit());
    return Params.Call(this) ? Params.Take<float>(L"ReturnValue") : 0.0f;
}

void UGameUserSettings::ApplyNonResolutionSettings()
{
    Reflection::FParams Params(StaticFunction_ApplyNonResolutionSettings());
    Params.Call(this);
}
