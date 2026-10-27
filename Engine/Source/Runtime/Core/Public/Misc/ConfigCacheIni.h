#pragma once

#include "Containers/Map.h"
#include "Containers/UnrealString.h"

#include <cstdint>

enum class EConfigFileHierarchy : uint8_t
{
    AbsoluteBase = 0,
    EngineDirBase,
    EngineDir_BasePlatform,
    EngineDirBase_NotForLicensees,
    EngineDirBase_NoRedist,
    GameDirDefault,
    GameDirDedicatedServer,
    GameDirDefault_NotForLicensees,
    GameDirDefault_NoRedist,
    EngineDir_Platform,
    EngineDir_Platform_NotForLicensees,
    EngineDir_Platform_NoRedist,
    GameDir_Platform,
    GameDir_Platform_NotForLicensees,
    GameDir_Platform_NoRedist,
    UserSettingsDir_EngineDir_User,
    UserDir_User,
    GameDir_User,
    NumLevels,
};

struct FIniFilename
{
    FString Filename;
    bool bRequired;
    FString CacheKey;

    FIniFilename(const FString& InFilename, bool InIsRequired, FString InCacheKey = FString(L""))
        : Filename(InFilename), bRequired(InIsRequired), CacheKey(InCacheKey)
    {
    }
};

static_assert(sizeof(FIniFilename) == 0x28);

typedef TMap<EConfigFileHierarchy, FIniFilename> FConfigFileHierarchy;

class FConfigCacheIni
{
public:
    static void Hook();

    // Engine voice chat (UE's VOIP through the game server) for builds whose desktop config turns it off: forces
    // [OnlineSubsystem] bHasVoiceEnabled on, and with bLocalVoice the voice module ([Voice] bEnabled) that captures and
    // plays it.
    static void EnableVoice(bool bLocalVoice);
};
