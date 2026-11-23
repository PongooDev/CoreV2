#pragma once

#include <cstdint>

class FPakPlatformFile
{
public:
    static const wchar_t* GetTypeName()
    {
        return L"PakFile";
    }

    bool Mount(const wchar_t* InPakFilename, uint32_t PakOrder, const wchar_t* InPath = nullptr);

    // 8.40+: mounts the server assets pak the launcher installs, once PreInit's own mount of it has failed. Run on the
    // engine's thread before UEngine::Init (a packed build's InstallHooks); does nothing on older builds or without the pak.
    static void MountServerAssets();
};
