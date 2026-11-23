#include "pch.h"
#include "HAL/PlatformFilemanager.h"
#include "Natives.h"

FPlatformFileManager& FPlatformFileManager::Get()
{
    return Natives::FPlatformFileManager_Get();
}

IPlatformFile* FPlatformFileManager::FindPlatformFile(const wchar_t* Name)
{
    return Natives::FPlatformFileManager_FindPlatformFile(this, Name);
}
