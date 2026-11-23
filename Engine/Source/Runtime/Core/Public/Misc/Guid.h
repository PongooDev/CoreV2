#pragma once

#include "Misc/Crc.h"

#include <cstdint>

class FString;

struct FGuid
{
    static FGuid NewGuid();
    // 32 hex digits, the hyphenated 8-4-4-4-12 form, or either inside braces or parentheses (UE's Digits,
    // DigitsWithHyphens, DigitsWithHyphensInBraces and DigitsWithHyphensInParentheses).
    static bool Parse(const FString& GuidString, FGuid& OutGuid);

    uint32_t A = 0;
    uint32_t B = 0;
    uint32_t C = 0;
    uint32_t D = 0;

    bool IsValid() const { return (A | B | C | D) != 0; }
    bool operator==(const FGuid& Other) const { return A == Other.A && B == Other.B && C == Other.C && D == Other.D; }
    bool operator!=(const FGuid& Other) const { return !(*this == Other); }

    friend uint32_t GetTypeHash(const FGuid& Guid) { return FCrc::MemCrc_DEPRECATED(&Guid, sizeof(FGuid)); }
};

static_assert(sizeof(FGuid) == 0x10);
