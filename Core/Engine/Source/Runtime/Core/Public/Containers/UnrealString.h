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
};
