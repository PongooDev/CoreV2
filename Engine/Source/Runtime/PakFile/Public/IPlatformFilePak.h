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

    // 8.40+ fallback: mounts the server assets pak the launcher installs, once PreInit's own mount of it has failed. Run
    // on the engine's thread before UEngine::Init (a packed build's InstallHooks); does nothing on older builds, without
    // the pak, or when the pak-signing takeover (TakeOverPakSigning) has already mounted it.
    static void MountServerAssets();

    // 8.40+ (owner-approved exception, 2026-11-24): take over FPakPlatformFile's OpenRead vtable slot so the engine
    // treats paks as unsigned. CL-6005771 took UE 4.24's pak signing, which rejects every pak whose master signature
    // isn't Epic's and so drops Core's config and server-assets paks at PreInit; the owner OK'd turning signing off for
    // unsigned paks to load on client and server. Called at CoreRedirect's stub handover, before the startup mounts;
    // installs a .rdata vtable swap. Does nothing on older builds (1.1-8.30) or when the finder fails.
    static void TakeOverPakSigning();

    // The body the OpenRead thunk runs once, on the first read routed through the pak platform file (right after
    // it becomes active): turns bSigned off for good on this instance and mounts Core's paks that PreInit rejected.
    static void TurnOffSigningAndRemount(FPakPlatformFile* This);
};
