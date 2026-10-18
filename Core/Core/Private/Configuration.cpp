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
        if (EqualsNoCase(Arguments[Index], L"-bNoConsole"))
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
