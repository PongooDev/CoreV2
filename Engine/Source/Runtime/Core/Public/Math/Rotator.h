#pragma once

#include "Math/Vector.h"

struct FRotator
{
    float Pitch = 0.0f;
    float Yaw = 0.0f;
    float Roll = 0.0f;

    FRotator() = default;

    constexpr FRotator(float InPitch, float InYaw, float InRoll)
        : Pitch(InPitch), Yaw(InYaw), Roll(InRoll)
    {
    }
};

static_assert(sizeof(FRotator) == 0xC);

inline FRotator FVector::Rotation() const
{
    constexpr float RadiansToDegrees = 180.0f / 3.1415926535897932f;
    return FRotator(FMath::Atan2(Z, FMath::Sqrt(X * X + Y * Y)) * RadiansToDegrees, FMath::Atan2(Y, X) * RadiansToDegrees, 0.0f);
}
