#include "pch.h"
#include "Misc/ConfigCacheIni.h"
#include "CoreGlobals.h"
#include "Finder.h"
#include "Hooks.h"
#include "Log.h"
#include "Natives.h"

#include <cwchar>

namespace
{
    using FGetSourceIniHierarchyFilenames = void (*)(const wchar_t*, const wchar_t*, const wchar_t*, const wchar_t*, FConfigFileHierarchy&, bool);
    FGetSourceIniHierarchyFilenames GetSourceIniHierarchyFilenamesOG = nullptr;

    void GetSourceIniHierarchyFilenames(const wchar_t* InBaseIniName, const wchar_t* InPlatformName, const wchar_t* EngineConfigDir, const wchar_t* SourceConfigDir, FConfigFileHierarchy& OutHierarchy, bool bRequireDefaultIni)
    {
        GetSourceIniHierarchyFilenamesOG(InBaseIniName, InPlatformName, EngineConfigDir, SourceConfigDir, OutHierarchy, bRequireDefaultIni);

        if (IsRunningDedicatedServer())
        {
            OutHierarchy.Add(EConfigFileHierarchy::GameDirDedicatedServer, FIniFilename(FString::Printf(TEXT("%s/DedicatedServer%s.ini"), SourceConfigDir, InBaseIniName), false));
            OutHierarchy.KeySort([](const EConfigFileHierarchy& A, const EConfigFileHierarchy& B) { return A < B; });
        }
    }

    using FGetBool = bool (*)(void*, const wchar_t*, const wchar_t*, bool&, const FString&);
    FGetBool GetBoolOG = nullptr;
    bool bForceLocalVoice = false;

    // 1.7.2's DefaultEngine.ini ships "bHasVoiceEnabled=false" and "[Voice] bEnabled=false" for desktop (DefaultGame.ini:
    // "Voice chat remains disabled on desktop platforms"); its DedicatedServerEngine.ini turns the first back on so
    // servers forward the consoles' voice.
    bool GetBool(void* This, const wchar_t* Section, const wchar_t* Key, bool& Value, const FString& Filename)
    {
        const bool bFound = GetBoolOG(This, Section, Key, Value, Filename);
        if (Section && Key)
        {
            const bool bVoiceInterface = std::wcscmp(Section, L"OnlineSubsystem") == 0 && std::wcscmp(Key, L"bHasVoiceEnabled") == 0;
            const bool bVoiceModule = bForceLocalVoice && std::wcscmp(Section, L"Voice") == 0 && std::wcscmp(Key, L"bEnabled") == 0;
            if (bVoiceInterface || bVoiceModule)
            {
                Value = true;
                return true;
            }
        }
        return bFound;
    }
}

void FConfigCacheIni::Hook()
{
    Hooks::Detour(L"GetSourceIniHierarchyFilenames", reinterpret_cast<void*>(Natives::GetSourceIniHierarchyFilenames), &GetSourceIniHierarchyFilenames, &GetSourceIniHierarchyFilenamesOG);
}

void FConfigCacheIni::EnableVoice(bool bLocalVoice)
{
    if (!Finder::ResolveVoiceConfig())
    {
        Log::Warning(L"Voice chat: config reader not found, the build's voice settings stay as shipped");
        return;
    }
    bForceLocalVoice = bLocalVoice;
    Hooks::Detour(L"FConfigCacheIni::GetBool", reinterpret_cast<void*>(Natives::FConfigCacheIni_GetBool), &GetBool, &GetBoolOG);
}
