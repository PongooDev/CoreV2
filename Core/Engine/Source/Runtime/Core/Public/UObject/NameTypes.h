#pragma once

#include "Containers/UnrealString.h"
#include "UObject/UnrealNames.h"

#include <functional>
#include <string>

enum EFindName
{
    FNAME_Find,
    FNAME_Add,
    FNAME_Replace_Not_Safe_For_Threading,
};

struct FName
{
    int32_t ComparisonIndex = 0;
    int32_t Number = 0;

    FName() = default;

    FName(EName N)
        : ComparisonIndex(N)
    {
    }

    FName(const wchar_t* Name, EFindName FindType = FNAME_Add);

    FString ToString() const;
    std::wstring ToStdString() const;

    bool IsNone() const
    {
        return ComparisonIndex == 0 && Number == 0;
    }

    bool operator==(const FName& Other) const = default;
};

template <>
struct std::hash<FName>
{
    size_t operator()(const FName& Name) const noexcept
    {
        return (static_cast<size_t>(static_cast<uint32_t>(Name.ComparisonIndex)) << 32) | static_cast<uint32_t>(Name.Number);
    }
};
