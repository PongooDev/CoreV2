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

    // ApproveServerSwitches found no whitelist string to change.
    bool bWhitelistNotFound = false;
    // ApproveServerSwitchesBeforeStub made the swap.
    bool bSwappedBeforeStub = false;

    constexpr wchar_t ClientWhitelist[] = L"-invitesession -invitefrom -party_joininfo_token -replay";
    constexpr wchar_t RebootWhitelist[] = L"-log -nosplash -nosound -nullrhi -useolditemcards       ";
    constexpr wchar_t ServerWhitelist[] = L"-log -nosplash -nosound -nullrhi -saveddirsuffix -unatt ";
    static_assert(sizeof(ClientWhitelist) == sizeof(ServerWhitelist) && sizeof(RebootWhitelist) == sizeof(ServerWhitelist),
        "the swap must keep the whitelist's length");
    constexpr size_t WhitelistSize = sizeof(ServerWhitelist) - sizeof(wchar_t);

    enum class EWhitelistSwap
    {
        AlreadyServer,
        Swapped,
        NotFound,
        WriteFailed,
    };

    // Puts the server's four switches in the place of the client's (or Reboot's) in the whitelist.
    EWhitelistSwap SwapWhitelist()
    {
        if (Finder::FindReadOnly(ServerWhitelist, WhitelistSize))
        {
            return EWhitelistSwap::AlreadyServer;
        }
        wchar_t* Whitelist = reinterpret_cast<wchar_t*>(Finder::FindReadOnly(ClientWhitelist, WhitelistSize));
        if (!Whitelist)
        {
            Whitelist = reinterpret_cast<wchar_t*>(Finder::FindReadOnly(RebootWhitelist, WhitelistSize));
        }
        if (!Whitelist)
        {
            return EWhitelistSwap::NotFound;
        }
        DWORD Protection = 0;
        if (!VirtualProtect(Whitelist, WhitelistSize, PAGE_READWRITE, &Protection))
        {
            return EWhitelistSwap::WriteFailed;
        }
        std::memcpy(Whitelist, ServerWhitelist, WhitelistSize);
        VirtualProtect(Whitelist, WhitelistSize, Protection, &Protection);
        return EWhitelistSwap::Swapped;
    }

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
    switch (SwapWhitelist())
    {
    case EWhitelistSwap::AlreadyServer:
        if (bSwappedBeforeStub)
        {
            Log::Info(L"FCommandLine: the whitelist Core changed before the packer's stub ran still lets the server's switches through");
        }
        else
        {
            Log::Info(L"FCommandLine: the exe already lets the server's switches through");
        }
        return;
    case EWhitelistSwap::NotFound:
        // 6.21 (CL-4526925) is built without the whitelist: its startup log prints "%sCommand Line: %s" with a constant
        // empty prefix (0x173EF86) where 6.20 picked "Filtered " (0x1728FA7), and FCommandLine keeps every switch. The
        // engine's command line, once set, tells which (CheckServerSwitches).
        bWhitelistNotFound = true;
        Log::Info(L"FCommandLine: no whitelist to change in this exe; the engine's command line is checked for -nullrhi once it is set");
        return;
    case EWhitelistSwap::WriteFailed:
        Log::Error(L"FCommandLine: can't write the whitelist ({})", GetLastError());
        return;
    case EWhitelistSwap::Swapped:
        Log::Info(L"FCommandLine: whitelisted -log -nosplash -nosound -nullrhi -saveddirsuffix -unattended");
        return;
    }
}

void FCommandLine::ApproveServerSwitchesBeforeStub()
{
    // A packed exe (1.9.1+) runs FCommandLine::Set soon after its stub hands over to the CRT, so the swap made after the
    // handover (ApproveServerSwitches, from Core's watcher) raced it: on a busy PC the swap came 0.3 to 3 s after the
    // handover and the engine had already dropped -nullrhi and -saveddirsuffix (9.21 CL-6922310, 8 of 22 test boots on
    // 2026-11-27, swaps 0.34 to 3.1 s late: a D3D11 device, the game's window on the desktop, logs in Saved instead of
    // Saved_CoreServer); every boot whose swap came within 0.31 s kept NullRHI. The stub decrypts only the first .text and leaves .rdata as the file
    // has it (9.21's whitelist is plain text at file offset 0x4967CF6), so the swap is made now, before the stub runs.
    // Where the whitelist isn't readable yet, the watcher's ApproveServerSwitches makes the swap after the handover.
    if (SwapWhitelist() == EWhitelistSwap::Swapped)
    {
        bSwappedBeforeStub = true;
        Log::Info(L"FCommandLine: whitelisted -log -nosplash -nosound -nullrhi -saveddirsuffix -unattended before the packer's stub runs");
    }
}

void FCommandLine::CheckServerSwitches()
{
    if (!bWhitelistNotFound || !HasSwitch(GetCommandLineW(), L"nullrhi"))
    {
        return;
    }
    const wchar_t* CommandLine = Get();
    if (CommandLine && HasSwitch(CommandLine, L"nullrhi"))
    {
        Log::Info(L"FCommandLine: the engine kept -nullrhi without a whitelist change: this exe has no whitelist");
    }
    else
    {
        Log::Warning(L"FCommandLine: whitelist not found and the engine dropped -nullrhi; -saveddirsuffix may be dropped too");
    }
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
