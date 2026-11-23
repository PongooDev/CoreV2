#include "pch.h"
#include "Misc/Base64.h"
#include "Containers/UnrealString.h"

namespace
{
    // The 6-bit value of a base64 character, '=' as 0, or -1 for anything else.
    int32_t DecodeChar(wchar_t Char)
    {
        if (Char >= L'A' && Char <= L'Z') return Char - L'A';
        if (Char >= L'a' && Char <= L'z') return Char - L'a' + 26;
        if (Char >= L'0' && Char <= L'9') return Char - L'0' + 52;
        if (Char == L'+') return 62;
        if (Char == L'/') return 63;
        if (Char == L'=') return 0;
        return -1;
    }
}

int32_t FBase64::GetDecodedDataSize(const FString& Source)
{
    const int32_t Length = Source.Len();
    if (Length % 4 != 0)
    {
        return 0;
    }
    int32_t NumBytes = Length / 4 * 3;
    if (Length > 0 && (*Source)[Length - 1] == L'=')
    {
        --NumBytes;
        if ((*Source)[Length - 2] == L'=')
        {
            --NumBytes;
        }
    }
    return NumBytes;
}

bool FBase64::Decode(const FString& Source, TArray<uint8_t>& OutDest)
{
    const int32_t Length = Source.Len();
    if (Length % 4 != 0)
    {
        return false;
    }
    const int32_t Size = GetDecodedDataSize(Source);
    OutDest.Reset();
    OutDest.AddUninitialized(Size);
    const wchar_t* Text = *Source;
    int32_t Written = 0;
    for (int32_t Index = 0; Index < Length; Index += 4)
    {
        uint32_t Group = 0;
        for (int32_t Char = 0; Char < 4; ++Char)
        {
            const int32_t Value = DecodeChar(Text[Index + Char]);
            if (Value < 0)
            {
                OutDest.Reset();
                return false;
            }
            Group = (Group << 6) | static_cast<uint32_t>(Value);
        }
        for (int32_t Byte = 0; Byte < 3 && Written < Size; ++Byte)
        {
            OutDest[Written++] = static_cast<uint8_t>(Group >> (16 - 8 * Byte));
        }
    }
    return true;
}
