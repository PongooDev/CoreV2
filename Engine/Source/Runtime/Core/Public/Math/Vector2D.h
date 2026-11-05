#pragma once

#include "Math/UnrealMathUtility.h"

// UE's FVector2D (Core/Public/Math/Vector2D.h): two floats.
struct FVector2D
{
    float X = 0.0f;
    float Y = 0.0f;

    FVector2D() = default;

    constexpr FVector2D(float InX, float InY)
        : X(InX), Y(InY)
    {
    }

    bool IsZero() const { return X == 0.0f && Y == 0.0f; }
};
