#include "pch.h"
#include "Athena/FortSafeZoneIndicator.h"
#include "ReflectionParams.h"

FVector AFortSafeZoneIndicator::GetSafeZoneCenter() const
{
    if (UFunction* Function = FindStaticFunction_GetSafeZoneCenter())
    {
        Reflection::FParams Params(Function, true);
        return Params.Call(const_cast<AFortSafeZoneIndicator*>(this)) ? Params.Take<FVector>(L"ReturnValue") : FVector();
    }
    // Up to 1.10 there is no such UFunction (1.11 adds it, with SafeZoneMesh): the indicator's Tick (1.9 0x480320)
    // moves the actor to the circle's centre as it sets Radius: LastCenter before the shrink, the lerp to NextCenter
    // during it, NextCenter after. 7.30's GetSafeZoneCenter (0xDD4EC0) likewise returns its mesh's location.
    return GetTransform().GetLocation();
}
