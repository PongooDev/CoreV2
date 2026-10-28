#pragma once

#include "Math/Quat.h"
#include "Math/Vector.h"

#include <cmath>

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

    FQuat Quaternion() const;
};

static_assert(sizeof(FRotator) == 0xC);

inline FRotator FVector::Rotation() const
{
    constexpr float RadiansToDegrees = 180.0f / 3.1415926535897932f;
    return FRotator(FMath::Atan2(Z, FMath::Sqrt(X * X + Y * Y)) * RadiansToDegrees, FMath::Atan2(Y, X) * RadiansToDegrees, 0.0f);
}

// 4.16's FRotator::Quaternion.
inline FQuat FRotator::Quaternion() const
{
    constexpr float HalfDegreesToRadians = 3.1415926535897932f / 360.0f;
    const float SP = std::sin(Pitch * HalfDegreesToRadians);
    const float CP = std::cos(Pitch * HalfDegreesToRadians);
    const float SY = std::sin(Yaw * HalfDegreesToRadians);
    const float CY = std::cos(Yaw * HalfDegreesToRadians);
    const float SR = std::sin(Roll * HalfDegreesToRadians);
    const float CR = std::cos(Roll * HalfDegreesToRadians);
    FQuat Result;
    Result.X = CR * SP * SY - SR * CP * CY;
    Result.Y = -CR * SP * CY - SR * CP * SY;
    Result.Z = CR * CP * SY - SR * SP * CY;
    Result.W = CR * CP * CY + SR * SP * SY;
    return Result;
}

inline float NormalizeRotatorAxis(float Angle)
{
    Angle = std::fmod(Angle, 360.0f);
    if (Angle < 0.0f)
    {
        Angle += 360.0f;
    }
    return Angle > 180.0f ? Angle - 360.0f : Angle;
}

inline FRotator FQuat::Rotator() const
{
    constexpr float SingularityThreshold = 0.4999995f;
    constexpr float RadiansToDegrees = 180.0f / 3.1415926535897932f;
    const float SingularityTest = Z * X - W * Y;
    const float YawY = 2.0f * (W * Z + X * Y);
    const float YawX = 1.0f - 2.0f * (Y * Y + Z * Z);
    FRotator Result;
    if (SingularityTest < -SingularityThreshold)
    {
        Result.Pitch = -90.0f;
        Result.Yaw = FMath::Atan2(YawY, YawX) * RadiansToDegrees;
        Result.Roll = NormalizeRotatorAxis(-Result.Yaw - 2.0f * FMath::Atan2(X, W) * RadiansToDegrees);
    }
    else if (SingularityTest > SingularityThreshold)
    {
        Result.Pitch = 90.0f;
        Result.Yaw = FMath::Atan2(YawY, YawX) * RadiansToDegrees;
        Result.Roll = NormalizeRotatorAxis(Result.Yaw - 2.0f * FMath::Atan2(X, W) * RadiansToDegrees);
    }
    else
    {
        Result.Pitch = std::asin(2.0f * SingularityTest) * RadiansToDegrees;
        Result.Yaw = FMath::Atan2(YawY, YawX) * RadiansToDegrees;
        Result.Roll = FMath::Atan2(-2.0f * (W * X + Y * Z), 1.0f - 2.0f * (X * X + Y * Y)) * RadiansToDegrees;
    }
    return Result;
}
