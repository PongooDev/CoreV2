#pragma once

#include "HAL/PlatformProperties.h"

class FOutputDeviceRedirector;

FOutputDeviceRedirector* GetGlobalLogSingleton();

#define GLog GetGlobalLogSingleton()

bool IsInGameThread();

inline bool IsRunningDedicatedServer()
{
    if (FPlatformProperties::IsServerOnly())
    {
        return true;
    }

    if (FPlatformProperties::IsGameOnly())
    {
        return false;
    }

    return false;
}

inline bool IsRunningClientOnly()
{
    return FPlatformProperties::IsClientOnly();
}
