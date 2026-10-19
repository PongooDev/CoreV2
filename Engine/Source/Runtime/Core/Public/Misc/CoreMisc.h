#pragma once

#include "CoreGlobals.h"
#include "HAL/PlatformProperties.h"

inline bool IsRunningGame()
{
    if (FPlatformProperties::IsGameOnly())
    {
        return true;
    }

    if (FPlatformProperties::IsServerOnly())
    {
        return false;
    }

    return false;
}
