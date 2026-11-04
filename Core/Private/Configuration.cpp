#include "pch.h"
#include "Configuration.h"

#include <shellapi.h>
#include <algorithm>
#include <string>
#include <string_view>

namespace
{
    FCoreConfiguration Current;

    bool EqualsNoCase(std::wstring_view Left, std::wstring_view Right)
    {
        return Left.size() == Right.size() && _wcsnicmp(Left.data(), Right.data(), Left.size()) == 0;
    }
}

void Configuration::Load()
{
    int32_t ArgumentCount = 0;
    wchar_t** Arguments = CommandLineToArgvW(GetCommandLineW(), &ArgumentCount);
    if (!Arguments)
    {
        return;
    }

    for (int32_t Index = 1; Index < ArgumentCount; ++Index)
    {
        const std::wstring_view Argument = Arguments[Index];
        constexpr std::wstring_view ModeOption = L"-CoreMode=";
        if (Argument.size() > ModeOption.size() && EqualsNoCase(Argument.substr(0, ModeOption.size()), ModeOption))
        {
            Current.Mode = Argument.substr(ModeOption.size());
        }
        if (EqualsNoCase(Argument, L"-CoreBotMatch"))
        {
            Current.bBotMatch = true;
        }
        if (EqualsNoCase(Argument, L"-CoreBotNoNavigation"))
        {
            Current.bBotNoNavigation = true;
        }
        if (EqualsNoCase(Argument, L"-CoreBotHotDrop"))
        {
            Current.bBotHotDrop = true;
        }
        constexpr std::wstring_view TeamSizeOption = L"-CoreBotTeamSize=";
        if (Argument.size() > TeamSizeOption.size() && EqualsNoCase(Argument.substr(0, TeamSizeOption.size()), TeamSizeOption))
        {
            Current.BotTeamSize = std::clamp(_wtoi(std::wstring(Argument.substr(TeamSizeOption.size())).c_str()), 1, 4);
        }
        constexpr std::wstring_view MapOption = L"-CoreMap=";
        if (Argument.size() > MapOption.size() && EqualsNoCase(Argument.substr(0, MapOption.size()), MapOption))
        {
            Current.Map = Argument.substr(MapOption.size());
        }
        constexpr std::wstring_view BackendOption = L"-CoreBackend=";
        if (Argument.size() > BackendOption.size() && EqualsNoCase(Argument.substr(0, BackendOption.size()), BackendOption))
        {
            Current.Backend = Argument.substr(BackendOption.size());
            while (!Current.Backend.empty() && Current.Backend.back() == L'/')
            {
                Current.Backend.pop_back();
            }
            // http://host[:port] or https://host[:port] (Core #197: a backend behind an https reverse proxy, on 443
            // unless a port is given). A bare host[:port] is http, as before.
            if (Current.Backend.find(L"://") == std::wstring::npos)
            {
                Current.Backend.insert(0, L"http://");
            }
            for (size_t Index = 0, End = Current.Backend.find(L"://"); Index < End; ++Index)
            {
                Current.Backend[Index] = static_cast<wchar_t>(towlower(Current.Backend[Index]));
            }
        }
    }

    LocalFree(Arguments);
}

bool FCoreConfiguration::IsBattleRoyale() const
{
    return EqualsNoCase(Mode, L"BattleRoyale");
}

const FCoreConfiguration& Configuration::Get()
{
    return Current;
}
