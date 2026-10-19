#include "pch.h"
#include "Misc/ConfigCacheIni.h"
#include "CoreGlobals.h"
#include "Hooks.h"
#include "Natives.h"

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
}

void FConfigCacheIni::Hook()
{
    Hooks::Detour(L"GetSourceIniHierarchyFilenames", reinterpret_cast<void*>(Natives::GetSourceIniHierarchyFilenames), &GetSourceIniHierarchyFilenames, &GetSourceIniHierarchyFilenamesOG);
}
