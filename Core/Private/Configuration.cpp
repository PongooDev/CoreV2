#include "pch.h"
#include "Configuration.h"

#include <shellapi.h>
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
        constexpr std::wstring_view BackendOption = L"-CoreBackend=";
        if (EqualsNoCase(Argument, L"-bNoConsole"))
        {
            Current.bConsole = false;
        }
        else if (Argument.size() > BackendOption.size() && EqualsNoCase(Argument.substr(0, BackendOption.size()), BackendOption))
        {
            Current.Backend = Argument.substr(BackendOption.size());
            while (!Current.Backend.empty() && Current.Backend.back() == L'/')
            {
                Current.Backend.pop_back();
            }
        }
    }

    LocalFree(Arguments);
}

const FCoreConfiguration& Configuration::Get()
{
    return Current;
}
