#include "pch.h"
#include "Misc/ConfigCacheIni.h"
#include "CoreGlobals.h"
#include "Finder.h"
#include "Hooks.h"
#include "Log.h"
#include "Natives.h"

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>

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

    // The value of a switch on the process's command line, as FParse::Value finds it: anywhere, ignoring case, not part
    // of a longer name, and quoted or up to the next space.
    std::optional<std::wstring> CommandLineValue(std::wstring_view Name)
    {
        const std::wstring_view CommandLine = GetCommandLineW();
        const auto Lower = [](wchar_t C) { return static_cast<wchar_t>(std::towlower(C)); };
        for (size_t At = 0; At + Name.size() <= CommandLine.size(); ++At)
        {
            if (!std::equal(Name.begin(), Name.end(), CommandLine.begin() + At, [&](wchar_t A, wchar_t B) { return Lower(A) == Lower(B); }))
            {
                continue;
            }
            if (At > 0 && (std::iswalnum(CommandLine[At - 1]) || CommandLine[At - 1] == L'_'))
            {
                continue;
            }

            size_t Start = At + Name.size();
            const bool bQuoted = Start < CommandLine.size() && CommandLine[Start] == L'"';
            Start += bQuoted ? 1 : 0;
            const size_t End = CommandLine.find_first_of(bQuoted ? L"\"" : L" \t", Start);
            return std::wstring(CommandLine.substr(Start, End == std::wstring_view::npos ? std::wstring_view::npos : End - Start));
        }
        return std::nullopt;
    }

    // The client's saved Engine.ini, the file the engine layers over the build's own config when it loads GEngineIni
    // (GenerateDestIniFile, 1.9.1 0x1318E10: keys in it win over the hierarchy's). FPaths::ProjectSavedDir (0x1340690):
    // the user dir, then "Saved", or "Saved_" and -saveddirsuffix's letters, digits and underscores. The user dir
    // (0x1340940) is %LOCALAPPDATA%/FortniteGame for an installed game, which these builds are (their Saved folder is
    // never next to the exe). -EngineINI= names the file instead (GetDestIniFilename, 0x131A500).
    std::optional<std::wstring> SavedEngineIni()
    {
        if (auto Override = CommandLineValue(L"EngineINI="))
        {
            return Override;
        }

        wchar_t LocalAppData[MAX_PATH] = {};
        const DWORD Length = GetEnvironmentVariableW(L"LOCALAPPDATA", LocalAppData, MAX_PATH);
        if (Length == 0 || Length >= MAX_PATH)
        {
            return std::nullopt;
        }

        std::wstring Saved = L"Saved";
        if (const auto Suffix = CommandLineValue(L"-saveddirsuffix="))
        {
            std::wstring Clean;
            std::copy_if(Suffix->begin(), Suffix->end(), std::back_inserter(Clean), [](wchar_t C) { return C == L'_' || (C < 0x80 && std::iswalnum(C)); });
            if (!Clean.empty())
            {
                Saved += L"_" + Clean;
            }
        }
        return std::wstring(LocalAppData) + L"\\FortniteGame\\" + Saved + L"\\Config\\WindowsClient\\Engine.ini";
    }

    bool IsTrue(const std::wstring& Path, const wchar_t* Section, const wchar_t* Key)
    {
        wchar_t Value[16] = {};
        GetPrivateProfileStringW(Section, Key, L"", Value, static_cast<DWORD>(std::size(Value)), Path.c_str());
        return _wcsicmp(Value, L"True") == 0 || std::wcscmp(Value, L"1") == 0;
    }
}

void FConfigCacheIni::WriteVoiceSettings(bool bLocalVoice)
{
    const auto Path = SavedEngineIni();
    if (!Path)
    {
        Log::Warning(L"Voice chat: no saved config folder (LOCALAPPDATA unset), the build's voice settings stay as shipped");
        return;
    }

    struct FSetting
    {
        const wchar_t* Section;
        const wchar_t* Key;
        bool bWanted;
    };
    // The same switches GetBool forces on the other builds.
    const FSetting Settings[] = {
        { L"OnlineSubsystem", L"bHasVoiceEnabled", true },
        { L"Voice", L"bEnabled", bLocalVoice },
    };

    bool bWrote = false;
    for (const FSetting& Setting : Settings)
    {
        if (!Setting.bWanted || IsTrue(*Path, Setting.Section, Setting.Key))
        {
            continue;
        }

        // A first launch has no saved config yet. Kernel32 only: this runs under the loader lock.
        for (size_t Separator = Path->find(L'\\', 3); Separator != std::wstring::npos; Separator = Path->find(L'\\', Separator + 1))
        {
            CreateDirectoryW(Path->substr(0, Separator).c_str(), nullptr);
        }
        // Only this key changes: the file's other sections and keys, the player's own settings, stay as they are.
        if (!WritePrivateProfileStringW(Setting.Section, Setting.Key, L"True", Path->c_str()))
        {
            Log::Warning(L"Voice chat: could not write [{}] {} to {} ({})", Setting.Section, Setting.Key, *Path, GetLastError());
            continue;
        }
        bWrote = true;
    }
    Log::Info(L"Voice chat: {} {}", bWrote ? L"turned voice on in" : L"voice already on in", *Path);
}

bool FConfigCacheIni::CombineIntoEngineIni(const wchar_t* What, const wchar_t* Text)
{
    if (!Finder::ResolveConfigCombine() || !*Natives::GConfig)
    {
        Log::Error(L"{}: can't merge into the engine's config (not found, or no GConfig yet)", What);
        return false;
    }

    const std::wstring_view EngineIni = **Natives::GEngineIni;
    if (EngineIni.size() < 10 || _wcsicmp(EngineIni.substr(EngineIni.size() - 10).data(), L"Engine.ini") != 0)
    {
        Log::Error(L"{}: GEngineIni is \"{}\", not an Engine.ini", What, EngineIni);
        return false;
    }

    void* File = Natives::FConfigCacheIni_Find(*Natives::GConfig, *Natives::GEngineIni, false);
    if (!File)
    {
        Log::Error(L"{}: {} isn't loaded", What, EngineIni);
        return false;
    }
    Natives::FConfigFile_CombineFromBuffer(File, FString(Text));
    Log::Info(L"{}: merged into {}", What, EngineIni);
    return true;
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
