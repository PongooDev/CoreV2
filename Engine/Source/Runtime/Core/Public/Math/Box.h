#pragma once

#include "Math/Vector.h"

#include <algorithm>

// An axis aligned box (Engine's FBox: Min, Max and IsValid, 0x1C).
struct FBox
{
    FVector Min;
    FVector Max;
    uint8_t IsValid = 0;

    FBox() = default;

    FBox(const FVector& InMin, const FVector& InMax)
        : Min(InMin), Max(InMax), IsValid(1)
    {
    }

    FVector GetCenter() const { return (Min + Max) * 0.5f; }
    FVector GetExtent() const { return (Max - Min) * 0.5f; }

    FBox& operator+=(const FVector& Point)
    {
        if (!IsValid)
        {
            Min = Max = Point;
            IsValid = 1;
            return *this;
        }
        Min = FVector(std::min(Min.X, Point.X), std::min(Min.Y, Point.Y), std::min(Min.Z, Point.Z));
        Max = FVector(std::max(Max.X, Point.X), std::max(Max.Y, Point.Y), std::max(Max.Z, Point.Z));
        return *this;
    }

    FBox& operator+=(const FBox& Other)
    {
        if (Other.IsValid)
        {
            *this += Other.Min;
            *this += Other.Max;
        }
        return *this;
    }

    bool IsInside(const FVector& Point) const
    {
        return IsValid && Point.X > Min.X && Point.X < Max.X && Point.Y > Min.Y && Point.Y < Max.Y && Point.Z > Min.Z && Point.Z < Max.Z;
    }
};

static_assert(sizeof(FBox) == 0x1C);
