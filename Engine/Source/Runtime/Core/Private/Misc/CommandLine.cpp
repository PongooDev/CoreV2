#include "pch.h"
#include "Misc/CommandLine.h"
#include "Log.h"
#include "Natives.h"

#include <shellapi.h>
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
