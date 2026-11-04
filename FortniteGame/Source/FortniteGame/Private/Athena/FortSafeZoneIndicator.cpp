#include "pch.h"
#include "Athena/FortSafeZoneIndicator.h"
#include "ReflectionParams.h"

FVector AFortSafeZoneIndicator::GetSafeZoneCenter() const
{
    Reflection::FParams Params(FindStaticFunction_GetSafeZoneCenter(), true);
    return Params.Call(const_cast<AFortSafeZoneIndicator*>(this)) ? Params.Take<FVector>(L"ReturnValue") : FVector();
}
