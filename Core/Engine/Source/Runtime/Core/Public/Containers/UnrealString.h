#pragma once

#include "Containers/Array.h"

#include <string>
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
