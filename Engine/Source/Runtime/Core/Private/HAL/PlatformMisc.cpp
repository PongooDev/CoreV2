#include "pch.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "Natives.h"

const wchar_t* FPlatformMisc::GameDir()
{
    return Natives::FPlatformMisc_GameDir();
}

const wchar_t* FPlatformProcess::UserName(bool bOnlyAlphaNumeric)
{
    return Natives::FPlatformProcess_UserName(bOnlyAlphaNumeric);
}
