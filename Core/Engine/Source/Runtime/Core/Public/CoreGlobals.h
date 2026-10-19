#pragma once

#include "HAL/PlatformProperties.h"

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
