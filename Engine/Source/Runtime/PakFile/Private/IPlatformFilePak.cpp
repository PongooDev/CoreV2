#include "pch.h"
#include "IPlatformFilePak.h"
#include "Containers/UnrealString.h"
#include "Finder.h"
#include "HAL/PlatformFilemanager.h"
#include "Log.h"
#include "Natives.h"
#include "Packed.h"

namespace
{
    // 8.40 (CL-6005771) takes UE 4.24's pak signing: FChunkCacheWorker's constructor (0x2873FD0) empties the chunk
    // hashes when the master signature doesn't decrypt to the SHA-1 of them under the build's key, and
    // FPakFile::SetupSignedPakReader (0x288A200) then gives no reader ("Unable to create pak handle", "pak is invalid").
    // 8.30 (CL-5793395) only broadcast that failure and read the pak, which is how the zeroed master signature of the
    // .sig beside Core's pak got through; only Epic's private key can make one 8.40 accepts.
    constexpr uint32_t FirstRejectingMasterSignatureChangelist = 6005771;

    // The name Core-Launcher installs the build's server assets pak under, in the build's Content/Paks.
    constexpr const wchar_t* ServerAssetsPakName = L"pakchunkCoreServerAssets-WindowsClient_P.pak";

    // FPakPlatformFile::GetPakOrderFromPakFilePath's order for a pak in FPaths::ProjectContentDir() that isn't
    // "Paks/<Project>-*", as PreInit gives it; Mount adds the _P.pak bonus that puts it over the game's own paks.
    constexpr uint32_t ProjectContentPakOrder = 3;

    // The project's Content/Paks, where PreInit mounts the game's paks from: the exe is in <Project>/Binaries/Win64.
    FString ProjectPaksDir()
    {
        wchar_t ExePath[MAX_PATH] = {};
        wchar_t PaksDir[MAX_PATH] = {};
        const DWORD Length = GetModuleFileNameW(nullptr, ExePath, MAX_PATH);
        FString Path(ExePath);
        Path = Path.Left(Path.Find(L"\\", ESearchCase::IgnoreCase, ESearchDir::FromEnd) + 1) + TEXT("..\\..\\Content\\Paks\\");
        if (!Length || Length == MAX_PATH || !GetFullPathNameW(*Path, MAX_PATH, PaksDir, nullptr))
        {
            return FString();
        }
        return FString(PaksDir);
    }
}

bool FPakPlatformFile::Mount(const wchar_t* InPakFilename, uint32_t PakOrder, const wchar_t* InPath)
{
    return Natives::FPakPlatformFile_Mount(this, InPakFilename, PakOrder, InPath);
}

void FPakPlatformFile::MountServerAssets()
{
    if (Packed::Changelist() < FirstRejectingMasterSignatureChangelist)
    {
        return;
    }

    const FString PakFilename = ProjectPaksDir() + ServerAssetsPakName;
    if (GetFileAttributesW(*PakFilename) == INVALID_FILE_ATTRIBUTES)
    {
        Log::Info(L"Server assets: no {} installed", ServerAssetsPakName);
        return;
    }
    if (!Finder::ResolvePakPlatformFileMount())
    {
        Log::Error(L"Server assets: {} is installed, but FPakPlatformFile::Mount wasn't found, so it stays unmounted", ServerAssetsPakName);
        return;
    }
    FPakPlatformFile* PakPlatformFile = reinterpret_cast<FPakPlatformFile*>(FPlatformFileManager::Get().FindPlatformFile(GetTypeName()));
    if (!PakPlatformFile)
    {
        Log::Error(L"Server assets: no pak platform file, so {} stays unmounted", ServerAssetsPakName);
        return;
    }

    // Only the build's key signs a pak 8.40 reads signed, so Core's pak is mounted as an unsigned one: bSigned, which
    // Mount hands to the FPakFile it makes, is off for this one call. The game's own paks were all mounted signed in
    // PreInit, and any mounted later still are.
    bool& bSigned = *reinterpret_cast<bool*>(reinterpret_cast<uint8_t*>(PakPlatformFile) + Natives::FPakPlatformFile_bSigned);
    const bool bWasSigned = bSigned;
    bSigned = false;
    const bool bMounted = PakPlatformFile->Mount(*PakFilename, ProjectContentPakOrder);
    bSigned = bWasSigned;

    if (bMounted)
    {
        Log::Info(L"Server assets: mounted {} unsigned (this build rejects its zeroed master signature)", ServerAssetsPakName);
    }
    else
    {
        Log::Error(L"Server assets: FPakPlatformFile::Mount refused {}", ServerAssetsPakName);
    }
}
