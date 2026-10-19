#pragma once

#include "Containers/Array.h"
#include "Misc/CString.h"

#include <string>
#include <climits>
#include <cwctype>
#include <string_view>

class FString : public TArray<wchar_t>
{
public:
    FString() = default;

    FString(const wchar_t* Text)
        : FString(std::wstring_view(Text ? Text : L""))
    {
    }

    FString(std::wstring_view Text)
    {
        if (!Text.empty())
        {
            Reserve(static_cast<int32_t>(Text.size()) + 1);
            memcpy(Data, Text.data(), Text.size() * sizeof(wchar_t));
            Data[Text.size()] = L'\0';
            ArrayNum = static_cast<int32_t>(Text.size()) + 1;
        }
    }

    const wchar_t* operator*() const
    {
        return ArrayNum ? Data : L"";
    }

    int32_t Len() const
    {
        return ArrayNum ? ArrayNum - 1 : 0;
    }

    bool IsEmpty() const
    {
        return Len() == 0;
    }

    std::wstring ToStdString() const
    {
        return std::wstring(**this, Len());
    }

    void Append(const wchar_t* Text, int32_t Count)
    {
        if (Count <= 0)
        {
            return;
        }

        const int32_t OldLen = Len();
        Reserve(OldLen + Count + 1);
        memcpy(Data + OldLen, Text, Count * sizeof(wchar_t));
        Data[OldLen + Count] = L'\0';
        ArrayNum = OldLen + Count + 1;
    }

    void AppendChar(wchar_t Char)
    {
        Append(&Char, 1);
    }

    FString& operator+=(const wchar_t* Text)
    {
        Append(Text, Text ? static_cast<int32_t>(wcslen(Text)) : 0);
        return *this;
    }

    FString& operator+=(const FString& Other)
    {
        Append(*Other, Other.Len());
        return *this;
    }

    FString& operator+=(wchar_t Char)
    {
        AppendChar(Char);
        return *this;
    }

    int32_t Find(const wchar_t* SubStr, ESearchCase::Type SearchCase = ESearchCase::IgnoreCase, ESearchDir::Type SearchDir = ESearchDir::FromStart, int32_t StartPosition = -1) const
    {
        const int32_t SubLen = SubStr ? static_cast<int32_t>(wcslen(SubStr)) : 0;
        const int32_t TextLen = Len();
        if (SubLen == 0 || SubLen > TextLen)
        {
            return -1;
        }

        const auto Matches = [&](int32_t Position)
        {
            for (int32_t Index = 0; Index < SubLen; ++Index)
            {
                const wchar_t A = Data[Position + Index];
                const wchar_t B = SubStr[Index];
                if (SearchCase == ESearchCase::CaseSensitive ? A != B : std::towupper(A) != std::towupper(B))
                {
                    return false;
                }
            }
            return true;
        };

        if (SearchDir == ESearchDir::FromStart)
        {
            for (int32_t Position = StartPosition < 0 ? 0 : StartPosition; Position <= TextLen - SubLen; ++Position)
            {
                if (Matches(Position))
                {
                    return Position;
                }
            }
        }
        else
        {
            const int32_t Last = StartPosition < 0 || StartPosition > TextLen - SubLen ? TextLen - SubLen : StartPosition;
            for (int32_t Position = Last; Position >= 0; --Position)
            {
                if (Matches(Position))
                {
                    return Position;
                }
            }
        }

        return -1;
    }

    FString Mid(int32_t Start, int32_t Count = INT_MAX) const
    {
        const int32_t TextLen = Len();
        const int32_t First = Start < 0 ? 0 : (Start > TextLen ? TextLen : Start);
        const int32_t Available = TextLen - First;
        const int32_t Taken = Count < 0 ? 0 : (Count > Available ? Available : Count);

        FString Result;
        Result.Append(**this + First, Taken);
        return Result;
    }

    FString Left(int32_t Count) const
    {
        return Mid(0, Count);
    }

    bool Split(const wchar_t* InS, FString* LeftS, FString* RightS, ESearchCase::Type SearchCase = ESearchCase::IgnoreCase, ESearchDir::Type SearchDir = ESearchDir::FromStart) const
    {
        const int32_t InPos = Find(InS, SearchCase, SearchDir);
        if (InPos < 0)
        {
            return false;
        }

        if (LeftS)
        {
            *LeftS = Left(InPos);
        }

        if (RightS)
        {
            *RightS = Mid(InPos + static_cast<int32_t>(wcslen(InS)));
        }

        return true;
    }

    friend FString operator+(FString Left, const FString& Right)
    {
        Left += Right;
        return Left;
    }

    friend FString operator+(FString Left, const wchar_t* Right)
    {
        Left += Right;
        return Left;
    }
};
