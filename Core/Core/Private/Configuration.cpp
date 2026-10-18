#include "pch.h"
#include "Configuration.h"

#include <shellapi.h>
#include <string_view>

namespace
{
    FCoreConfiguration Current;

    bool StartsWithNoCase(std::wstring_view Text, std::wstring_view Prefix)
    {
        return Text.size() >= Prefix.size() && _wcsnicmp(Text.data(), Prefix.data(), Prefix.size()) == 0;
    }

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

        if (StartsWithNoCase(Argument, L"MapToLoad="))
        {
            Current.MapToLoad = Argument.substr(10);
        }
        else if (StartsWithNoCase(Argument, L"Port="))
        {
            Current.Port = _wtoi(Argument.substr(5).data());
        }
        else if (EqualsNoCase(Argument, L"-bListenServer"))
        {
            Current.bListenServer = true;
        }
        else if (EqualsNoCase(Argument, L"-bNoConsole"))
        {
            Current.bConsole = false;
        }
    }

    LocalFree(Arguments);
}

const FCoreConfiguration& Configuration::Get()
{
    return Current;
}
