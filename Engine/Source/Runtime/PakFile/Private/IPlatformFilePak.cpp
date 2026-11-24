#include "pch.h"
#include "IPlatformFilePak.h"
#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "Finder.h"
#include "HAL/PlatformFilemanager.h"
#include "Hooks.h"
#include "Log.h"
#include "Natives.h"
#include "Packed.h"

#include <cwchar>

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

    bool HasPakExtension(const wchar_t* Name)
    {
        const size_t Len = Name ? wcslen(Name) : 0;
        return Len >= 4 && _wcsicmp(Name + Len - 4, L".pak") == 0;
    }

    // The lowercased file name (after the last separator) of a pak path, for case-insensitive comparison.
    FString LowerBaseName(const wchar_t* Path)
    {
        const wchar_t* Name = Path;
        for (const wchar_t* C = Path; C && *C; ++C)
        {
            if (*C == L'\\' || *C == L'/')
            {
                Name = C + 1;
            }
        }
        return FString(Name).ToLower();
    }

    // FString has no operator== in this tree, so compare the (already lowercased) names through their buffers.
    bool ContainsName(const TArray<FString>& Names, const FString& Name)
    {
        for (const FString& Existing : Names)
        {
            if (wcscmp(*Existing, *Name) == 0)
            {
                return true;
            }
        }
        return false;
    }

    // Every *.pak under Dir and its subfolders (Dir ends with a backslash).
    void FindPaksRecursively(const FString& Dir, TArray<FString>& OutPaks)
    {
        if (Dir.Len() == 0)
        {
            return;
        }
        WIN32_FIND_DATAW Find = {};
        const FString Search = Dir + TEXT("*");
        const HANDLE Handle = FindFirstFileW(*Search, &Find);
        if (Handle == INVALID_HANDLE_VALUE)
        {
            return;
        }
        do
        {
            if (wcscmp(Find.cFileName, L".") == 0 || wcscmp(Find.cFileName, L"..") == 0)
            {
                continue;
            }
            if (Find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                FindPaksRecursively(Dir + Find.cFileName + TEXT("\\"), OutPaks);
            }
            else if (HasPakExtension(Find.cFileName))
            {
                OutPaks.Add(Dir + Find.cFileName);
            }
        } while (FindNextFileW(Handle, &Find));
        FindClose(Handle);
    }

    // Adds the lowercased base name of the FString at String (wchar_t* Data at +0x0, int32 ArrayNum at +0x8) to Out.
    void AddFStringBaseName(const uint8_t* String, TArray<FString>& Out)
    {
        if (!String || !Finder::IsReadable(String, 0x10))
        {
            return;
        }
        const wchar_t* const Name = *reinterpret_cast<const wchar_t* const*>(String + 0x0);
        const int32_t NameLen = *reinterpret_cast<const int32_t*>(String + 0x8);
        if (!Name || NameLen <= 0 || NameLen > 1024 || !Finder::IsReadable(Name, static_cast<size_t>(NameLen) * sizeof(wchar_t)))
        {
            return;
        }
        Out.Add(LowerBaseName(Name));
    }

    // A TArray at Base+Offset (Data at +0x0, ArrayNum at +0x8): its data and count, or false when it can't be read.
    bool ReadArray(const uint8_t* Base, ptrdiff_t Offset, size_t Stride, const uint8_t*& OutData, int32_t& OutNum)
    {
        OutData = *reinterpret_cast<const uint8_t* const*>(Base + Offset);
        OutNum = *reinterpret_cast<const int32_t*>(Base + Offset + 0x8);
        if (OutNum == 0)
        {
            return true;
        }
        return OutData && OutNum > 0 && OutNum <= 8192 && Finder::IsReadable(OutData, static_cast<size_t>(OutNum) * Stride);
    }

    // The engine's own record of PakPlatformFile's paks, as lowercased base names: OutMounted from PakFiles (the paks it
    // mounted) and OutPending from PendingEncryptedPakFiles (the ones it deferred until their AES key is registered).
    // False when either list can't be read, so the caller mounts nothing rather than risk a duplicate of Epic's paks.
    // Layouts, checked on 8.40 in FPakPlatformFile::Mount (0x2883A10), the same as 21.40's:
    // - PakFiles: TArray<FPakListEntry> at +0x10; FPakListEntry { uint32 ReadOrder; FPakFile* PakFile } (stride 0x10,
    //   Mount's add at 0x2883E6C). On UE 4.22 FPakFile has no IPakFile base and begins with its PakFilename FString (its
    //   ctor 0x2874F90 builds it at +0x0).
    // - PendingEncryptedPakFiles: TArray<FPakListDeferredEntry> at +0x20; FPakListDeferredEntry { FString Filename;
    //   FString Path; uint32 ReadOrder; FGuid EncryptionKeyGuid; int32 PakchunkIndex } (stride 0x38, Mount's add after
    //   its "Deferring mount of pak" log at 0x2883F86).
    bool ReadEnginePakRecord(FPakPlatformFile* PakPlatformFile, TArray<FString>& OutMounted, TArray<FString>& OutPending)
    {
        const uint8_t* const Base = reinterpret_cast<const uint8_t*>(PakPlatformFile);
        const uint8_t* Data = nullptr;
        int32_t Num = 0;
        if (!ReadArray(Base, 0x10, 0x10, Data, Num) || Num == 0)
        {
            return false;  // the engine's own paks are always mounted by now; an empty or unreadable list is wrong
        }
        for (int32_t i = 0; i < Num; ++i)
        {
            const uint8_t* const Pak = *reinterpret_cast<const uint8_t* const*>(Data + static_cast<size_t>(i) * 0x10 + 0x8);
            AddFStringBaseName(Pak, OutMounted);
        }

        if (!ReadArray(Base, 0x20, 0x38, Data, Num))
        {
            return false;
        }
        for (int32_t i = 0; i < Num; ++i)
        {
            AddFStringBaseName(Data + static_cast<size_t>(i) * 0x38, OutPending);
        }
        return true;
    }

    // FPakPlatformFile_vtbl slot 25 (+0xC8) is OpenRead (21.40 FPakPlatformFile_vtbl order). CoreRedirect takes it over
    // rather than Initialize/InitializeAfterSetActive because those are called on the concrete FPakPlatformFile* on this
    // build (not through the vtable), while file operations always go through IPlatformFile*. OpenRead specifically,
    // because it is the first method the engine routes through the pak platform file (its first FileExists lands much
    // later), so the pak-signing takeover happens as early as the vtable allows.
    constexpr int32_t OpenReadSlot = 25;

    using FOpenRead = void* (*)(FPakPlatformFile*, const wchar_t*, bool);
    FOpenRead OriginalOpenRead = nullptr;
    bool bPakSigningTurnedOff = false;

    // Owner-approved exception (2026-11-24). Runs on the first OpenRead the engine routes through the pak platform file,
    // right after SetPlatformFile makes it the top platform file -- after PreInit's signed mount of the chain, and as the
    // config system starts reading the inis. It turns signing off (once) and mounts the paks PreInit rejected (the config
    // pak among them, so its NoRedist inis are in place for the hierarchy read that follows). Initialize (slot 4) and
    // InitializeAfterSetActive (slot 5) are called on the concrete type here, not through the vtable, so they can't be
    // taken over; and the pak-signing-keys delegate can't be located under the build's obfuscation to unbind it before
    // Initialize reads it, which is the only way to keep the startup mounts themselves unsigned (so the first "pak is
    // invalid" for Core's paks still appears in the log, then they mount here).
    void* OpenReadThunk(FPakPlatformFile* This, const wchar_t* Filename, bool bAllowWrite)
    {
        if (!bPakSigningTurnedOff)
        {
            bPakSigningTurnedOff = true;  // set first: TurnOffSigningAndRemount's own file reads re-enter this thunk
            FPakPlatformFile::TurnOffSigningAndRemount(This);

            // Put the engine's own pointer back so the vtable is pristine for the packer's later code check and every
            // further OpenRead runs the engine's method directly.
            if (Natives::FPakPlatformFile_VTable && OriginalOpenRead)
            {
                Hooks::SwapVTable(L"FPakPlatformFile::OpenRead (restore)", Natives::FPakPlatformFile_VTable,
                    OpenReadSlot, reinterpret_cast<void*>(OriginalOpenRead), nullptr);
            }
        }
        return OriginalOpenRead(This, Filename, bAllowWrite);
    }
}

bool FPakPlatformFile::Mount(const wchar_t* InPakFilename, uint32_t PakOrder, const wchar_t* InPath)
{
    return Natives::FPakPlatformFile_Mount(this, InPakFilename, PakOrder, InPath);
}

void FPakPlatformFile::TurnOffSigningAndRemount(FPakPlatformFile* This)
{
    // bSigned off for good: Mount hands it to every FPakFile it makes, so the paks mounted here and anything mounted later
    // (the deferred encrypted paks once their key is registered, Creative islands, streaming) are read unsigned. The
    // game's own signed paks were already mounted in PreInit.
    *reinterpret_cast<bool*>(reinterpret_cast<uint8_t*>(This) + Natives::FPakPlatformFile_bSigned) = false;

    // Mount every Content/Paks pak PreInit's signature check rejected (Core's paks, or one a user added): those on disk
    // that are neither in the engine's mounted list nor in its pending-key list. The deferred ones (the encrypted
    // chunk1000+ paks) are left to the engine, which mounts them when their AES key arrives -- unsigned now too.
    TArray<FString> Mounted;
    TArray<FString> Pending;
    if (!ReadEnginePakRecord(This, Mounted, Pending))
    {
        Log::Error(L"Pak signing: turned off, but the engine's mounted/pending pak lists can't be read, so no rejected pak is remounted");
        return;
    }

    TArray<FString> DiskPaks;
    FindPaksRecursively(ProjectPaksDir(), DiskPaks);

    int32_t Remounted = 0;
    for (const FString& PakPath : DiskPaks)
    {
        const FString BaseName = LowerBaseName(*PakPath);
        if (ContainsName(Mounted, BaseName) || ContainsName(Pending, BaseName))
        {
            continue;
        }
        if (This->Mount(*PakPath, ProjectContentPakOrder))  // Mount adds the _P.pak bonus the engine would have given it
        {
            ++Remounted;
            Mounted.Add(BaseName);
            Log::Info(L"Pak signing: mounted {} unsigned (PreInit rejected its master signature)", *BaseName);
        }
        else
        {
            Log::Error(L"Pak signing: FPakPlatformFile::Mount refused {}", *BaseName);
        }
    }
    Log::Info(L"Pak signing: turned off on the pak platform file; {} mounted, {} pending a key, remounted {} rejected pak(s)",
        Mounted.Num() - Remounted, Pending.Num(), Remounted);
}

void FPakPlatformFile::TakeOverPakSigning()
{
    if (Packed::Changelist() < FirstRejectingMasterSignatureChangelist)
    {
        return;  // 1.1-8.30 read unsigned paks already, so they are untouched
    }
    if (!Finder::ResolvePakPlatformFileMount())
    {
        Log::Error(L"Pak signing: FPakPlatformFile::Mount/bSigned not found, so signing stays on (unsigned paks won't load)");
        return;
    }
    if (!Finder::ResolvePakPlatformFileVTable())
    {
        Log::Error(L"Pak signing: FPakPlatformFile vtable not found, so signing stays on (unsigned paks won't load)");
        return;
    }

    // Owner-approved exception (2026-11-24): a .rdata vtable swap so the engine treats paks as unsigned on 8.40+. Runs at
    // the redirect's stub handover, before the engine constructs and uses the pak platform file, so the swap is in place
    // when OpenRead is first called through it. The slot is put back inside the thunk's one-shot, leaving the vtable
    // pristine for the packer's later code check.
    Hooks::SwapVTable(L"FPakPlatformFile::OpenRead", Natives::FPakPlatformFile_VTable, OpenReadSlot,
        reinterpret_cast<void*>(&OpenReadThunk), reinterpret_cast<void**>(&OriginalOpenRead));
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

    // Fallback: CoreRedirect's pak-signing takeover (TakeOverPakSigning) normally mounts this pak earlier, in the same
    // process; only mount it here when that didn't run (e.g. its finder failed). Skip if it's already mounted.
    TArray<FString> Mounted;
    TArray<FString> Pending;
    if (ReadEnginePakRecord(PakPlatformFile, Mounted, Pending) && ContainsName(Mounted, LowerBaseName(ServerAssetsPakName)))
    {
        Log::Info(L"Server assets: {} already mounted by the pak-signing takeover", ServerAssetsPakName);
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
