#pragma once

#include "Containers/UnrealString.h"
#include "UObject/UnrealNames.h"

#include <functional>
#include <string>

#define INVALID_LONGPACKAGE_CHARACTERS TEXT("\\:*?\"<>|' ,.&!~\n\r\t@#")

enum EFindName
{
    FNAME_Find,
    FNAME_Add,
    FNAME_Replace_Not_Safe_For_Threading,
};

struct alignas(8) FName
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

inline uint32_t GetTypeHash(const FName& Name)
{
    return static_cast<uint32_t>(Name.ComparisonIndex + Name.Number);
}

// An FName as UE 4.22 lays it out (7.00, CL-4629139): its two int32s with no uint64 beside them, so 4-byte aligned. Up to
// UE 4.21 a union with a uint64 made it 8-byte aligned, as Core's FName is. Only for reading an engine container whose
// elements hold an FName next to 4-byte members (TMap<FName, int32>: 0x14-byte elements there, 0x18 before), on the
// builds ObjectLayout::IsNameAligned4 says so.
struct FPackedName
{
    int32_t ComparisonIndex = 0;
    int32_t Number = 0;

    FPackedName() = default;

    FPackedName(const FName& Name)
        : ComparisonIndex(Name.ComparisonIndex), Number(Name.Number)
    {
    }

    bool operator==(const FPackedName& Other) const = default;
};

inline uint32_t GetTypeHash(const FPackedName& Name)
{
    return static_cast<uint32_t>(Name.ComparisonIndex + Name.Number);
}

template <>
struct std::hash<FName>
{
    size_t operator()(const FName& Name) const noexcept
    {
        return (static_cast<size_t>(static_cast<uint32_t>(Name.ComparisonIndex)) << 32) | static_cast<uint32_t>(Name.Number);
    }
};
