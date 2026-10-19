#pragma once

#include <cstdint>

struct FColor
{
    union
    {
        struct
        {
            uint8_t B;
            uint8_t G;
            uint8_t R;
            uint8_t A;
        };
        uint32_t AlignmentDummy;
    };

    FColor()
        : AlignmentDummy(0)
    {
    }

    constexpr FColor(uint8_t InR, uint8_t InG, uint8_t InB, uint8_t InA = 255)
        : B(InB), G(InG), R(InR), A(InA)
    {
    }

    uint32_t& DWColor() { return AlignmentDummy; }
    const uint32_t& DWColor() const { return AlignmentDummy; }

    bool operator==(const FColor& Other) const { return DWColor() == Other.DWColor(); }
};

static_assert(sizeof(FColor) == 4);
