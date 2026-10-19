#include "pch.h"
#include "Misc/DefaultValueHelper.h"

#include <cwctype>

bool FDefaultValueHelper::IsWhitespace(wchar_t Char)
{
    const bool bIsLinebreak = (Char >= 0x0A && Char <= 0x0D) || Char == 0x85 || Char == 0x2028 || Char == 0x2029;
    return std::iswspace(Char) || bIsLinebreak || Char == L'\r';
}

bool FDefaultValueHelper::HasWhitespaces(const FString& Source)
{
    for (int32_t Pos = 0; Pos < Source.Len(); ++Pos)
    {
        if (IsWhitespace(Source[Pos]))
        {
            return true;
        }
    }
    return false;
}
