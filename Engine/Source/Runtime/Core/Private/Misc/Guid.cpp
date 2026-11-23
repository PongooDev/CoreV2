#include "pch.h"
#include "Misc/Guid.h"
#include "Containers/UnrealString.h"

#include <combaseapi.h>

FGuid FGuid::NewGuid()
{
    FGuid Result;
    CoCreateGuid(reinterpret_cast<GUID*>(&Result));
    return Result;
}

namespace
{
    bool ParseHex(const wchar_t* Text, int32_t Count, uint32_t& OutValue)
    {
        uint32_t Value = 0;
        for (int32_t Index = 0; Index < Count; ++Index)
        {
            const wchar_t Char = Text[Index];
            uint32_t Digit = 0;
            if (Char >= L'0' && Char <= L'9') Digit = Char - L'0';
            else if (Char >= L'a' && Char <= L'f') Digit = Char - L'a' + 10;
            else if (Char >= L'A' && Char <= L'F') Digit = Char - L'A' + 10;
            else return false;
            Value = (Value << 4) | Digit;
        }
        OutValue = Value;
        return true;
    }

    // Digits: AAAAAAAABBBBBBBBCCCCCCCCDDDDDDDD.
    bool ParseDigits(const wchar_t* Text, FGuid& OutGuid)
    {
        FGuid Guid;
        if (!ParseHex(Text, 8, Guid.A) || !ParseHex(Text + 8, 8, Guid.B) || !ParseHex(Text + 16, 8, Guid.C) || !ParseHex(Text + 24, 8, Guid.D))
        {
            return false;
        }
        OutGuid = Guid;
        return true;
    }

    // DigitsWithHyphens: AAAAAAAA-BBBB-BBBB-CCCC-CCCCDDDDDDDD.
    bool ParseDigitsWithHyphens(const wchar_t* Text, FGuid& OutGuid)
    {
        if (Text[8] != L'-' || Text[13] != L'-' || Text[18] != L'-' || Text[23] != L'-')
        {
            return false;
        }
        uint32_t B1 = 0, B2 = 0, C1 = 0, C2 = 0;
        FGuid Guid;
        if (!ParseHex(Text, 8, Guid.A) || !ParseHex(Text + 9, 4, B1) || !ParseHex(Text + 14, 4, B2) || !ParseHex(Text + 19, 4, C1)
            || !ParseHex(Text + 24, 4, C2) || !ParseHex(Text + 28, 8, Guid.D))
        {
            return false;
        }
        Guid.B = (B1 << 16) | B2;
        Guid.C = (C1 << 16) | C2;
        OutGuid = Guid;
        return true;
    }
}

bool FGuid::Parse(const FString& GuidString, FGuid& OutGuid)
{
    const wchar_t* Text = *GuidString;
    switch (GuidString.Len())
    {
    case 32:
        return ParseDigits(Text, OutGuid);
    case 36:
        return ParseDigitsWithHyphens(Text, OutGuid);
    case 38:
        if ((Text[0] == L'{' && Text[37] == L'}') || (Text[0] == L'(' && Text[37] == L')'))
        {
            return ParseDigitsWithHyphens(Text + 1, OutGuid);
        }
        return false;
    default:
        return false;
    }
}
