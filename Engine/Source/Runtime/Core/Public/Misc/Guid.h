#pragma once

#include <cstdint>

struct FGuid
{
    static FGuid NewGuid();

    uint32_t A = 0;
    uint32_t B = 0;
    uint32_t C = 0;
    uint32_t D = 0;

    bool IsValid() const { return (A | B | C | D) != 0; }
    bool operator==(const FGuid& Other) const { return A == Other.A && B == Other.B && C == Other.C && D == Other.D; }
    bool operator!=(const FGuid& Other) const { return !(*this == Other); }
};

static_assert(sizeof(FGuid) == 0x10);
