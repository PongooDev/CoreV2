#pragma once

#include "Math/UnrealMathUtility.h"

struct FRotator;

struct FVector
{
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;

    FVector() = default;

    constexpr FVector(float InX, float InY, float InZ)
        : X(InX), Y(InY), Z(InZ)
    {
    }

    FVector operator+(const FVector& V) const { return FVector(X + V.X, Y + V.Y, Z + V.Z); }
    FVector operator-(const FVector& V) const { return FVector(X - V.X, Y - V.Y, Z - V.Z); }
    FVector operator*(float Scale) const { return FVector(X * Scale, Y * Scale, Z * Scale); }
    float operator|(const FVector& V) const { return X * V.X + Y * V.Y + Z * V.Z; }

    FRotator Rotation() const;

    float SizeSquared() const { return X * X + Y * Y + Z * Z; }
    float Size() const { return FMath::Sqrt(SizeSquared()); }

    static float DistSquared(const FVector& V1, const FVector& V2)
    {
        return FMath::Square(V2.X - V1.X) + FMath::Square(V2.Y - V1.Y) + FMath::Square(V2.Z - V1.Z);
    }
};

static_assert(sizeof(FVector) == 0xC);
