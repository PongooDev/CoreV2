#include "pch.h"
#include "Misc/CommandLine.h"
#include "Finder.h"
#include "Log.h"
#include "Natives.h"

#include <shellapi.h>
#include <cstring>
#include <cwchar>

namespace
{
    // FCommandLine's CmdLine buffer (UE 4.16 CommandLine.cpp: TCHAR CmdLine[16384]).
    constexpr size_t CommandLineCapacity = 16384;

    bool HasSwitch(const wchar_t* CommandLine, const wchar_t* Switch)
    {
        int32_t Count = 0;
        wchar_t** Arguments = CommandLineToArgvW(CommandLine, &Count);
        bool bFound = false;
        for (int32_t Index = 0; Arguments && Index < Count && !bFound; ++Index)
        {
            const wchar_t* Argument = Arguments[Index];
            bFound = (Argument[0] == L'-' || Argument[0] == L'/') && _wcsicmp(Argument + 1, Switch) == 0;
        }
        LocalFree(Arguments);
        return bFound;
    }
}

const wchar_t* FCommandLine::Get()
{
    return Natives::FCommandLine_Get();
}

void FCommandLine::RestoreSwitch(const wchar_t* Switch)
{
    // The shipping client exe keeps only whitelisted switches in FCommandLine (FCommandLine::Set → WhitelistCommandLines),
    // so a server switch such as -EnableMMS never reaches FParse::Param although the log's command line shows it.
    if (HasSwitch(GetCommandLineW(), Switch))
    {
        AddSwitch(Switch);
    }
}

void FCommandLine::ApproveServerSwitches()
{
    // The whitelist FCommandLine::Set keeps switches from is one space-separated string in .rdata ("... -epicapp -epicenv
    // -invitesession -invitefrom -party_joininfo_token -replay -purgatorymallocproxy ..."), read when the engine starts;
    // a switch is kept when it starts with one of them. Without -nullrhi in it a 1.8 server made a D3D11 device and
    // crashed building the Early Access loading screen (0x1248480, through a null Slate global). Reboot Launcher patches
    // the exe on disk to swap these four client switches for server ones (the launcher does too, ExePatcher); Core makes
    // the swap in memory first (it's loaded before the exe's entry point), so an unpatched exe works too. Core's swap
    // also lets -saveddirsuffix= through in place of Reboot's -useolditemcards: the launcher gives servers their own
    // saved folder with it (FPaths::GameSavedDir, 1.8 0x13058C0: Saved_<suffix>), whose Engine.ini turns XMPP off
    // (#109). The last place, "-unatt", lets -unattended through (a switch is kept when it starts with a whitelisted
    // one): without it a server shows the engine's message boxes, such as "Default Property warnings and errors", on the
    // host's desktop and waits for someone to close them. 1.1 and 1.2 have no whitelist.
    static constexpr wchar_t Client[] = L"-invitesession -invitefrom -party_joininfo_token -replay";
    static constexpr wchar_t Reboot[] = L"-log -nosplash -nosound -nullrhi -useolditemcards       ";
    static constexpr wchar_t Server[] = L"-log -nosplash -nosound -nullrhi -saveddirsuffix -unatt ";
    static_assert(sizeof(Client) == sizeof(Server) && sizeof(Reboot) == sizeof(Server), "the swap must keep the whitelist's length");
    constexpr size_t Size = sizeof(Server) - sizeof(wchar_t);

    if (Finder::FindReadOnly(Server, Size))
    {
        Log::Info(L"FCommandLine: the exe already lets the server's switches through");
        return;
    }
    wchar_t* Whitelist = reinterpret_cast<wchar_t*>(Finder::FindReadOnly(Client, Size));
    if (!Whitelist)
    {
        Whitelist = reinterpret_cast<wchar_t*>(Finder::FindReadOnly(Reboot, Size));
    }
    if (!Whitelist)
    {
        Log::Warning(L"FCommandLine: whitelist not found; -nullrhi and -saveddirsuffix may be dropped");
        return;
    }
    DWORD Protection = 0;
    if (!VirtualProtect(Whitelist, Size, PAGE_READWRITE, &Protection))
    {
        Log::Error(L"FCommandLine: can't write the whitelist ({})", GetLastError());
        return;
    }
    std::memcpy(Whitelist, Server, Size);
    VirtualProtect(Whitelist, Size, Protection, &Protection);
    Log::Info(L"FCommandLine: whitelisted -log -nosplash -nosound -nullrhi -saveddirsuffix -unattended");
}

void FCommandLine::AddSwitch(const wchar_t* Switch)
{
    wchar_t* CommandLine = const_cast<wchar_t*>(Get());
    if (!CommandLine || HasSwitch(CommandLine, Switch))
    {
        return;
    }

    const size_t Length = wcslen(CommandLine);
    const size_t Needed = Length + wcslen(Switch) + 2;
    if (Needed >= CommandLineCapacity)
    {
        Log::Error(L"FCommandLine: no room to restore -{}", Switch);
        return;
    }
    swprintf(CommandLine + Length, CommandLineCapacity - Length, L" -%s", Switch);
    Log::Info(L"FCommandLine: added -{}", Switch);
}
