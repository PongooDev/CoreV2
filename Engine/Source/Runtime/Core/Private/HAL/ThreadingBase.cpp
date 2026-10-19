#include "pch.h"
#include "CoreGlobals.h"
#include "Natives.h"

bool IsInGameThread()
{
    if (Natives::GIsGameThreadIdInitialized && *Natives::GIsGameThreadIdInitialized)
    {
        return GetCurrentThreadId() == *Natives::GGameThreadId;
    }

    return true;
}
