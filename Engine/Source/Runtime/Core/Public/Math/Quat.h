#pragma once

#include "Math/Vector.h"

struct FRotator;

struct alignas(16) FQuat
{
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
    float W = 1.0f;

    FRotator Rotator() const;

    FVector RotateVector(const FVector& V) const
    {
        const FVector Q(X, Y, Z);
        const FVector T = FVector::CrossProduct(Q, V) * 2.0f;
        return V + T * W + FVector::CrossProduct(Q, T);
    }

    FVector UnrotateVector(const FVector& V) const
    {
        const FVector Q(-X, -Y, -Z);
        const FVector T = FVector::CrossProduct(Q, V) * 2.0f;
        return V + T * W + FVector::CrossProduct(Q, T);
    }

    // Epic's: this rotation after Q's.
    FQuat operator*(const FQuat& Q) const
    {
        FQuat Result;
        Result.X = W * Q.X + X * Q.W + Y * Q.Z - Z * Q.Y;
        Result.Y = W * Q.Y - X * Q.Z + Y * Q.W + Z * Q.X;
        Result.Z = W * Q.Z + X * Q.Y - Y * Q.X + Z * Q.W;
        Result.W = W * Q.W - X * Q.X - Y * Q.Y - Z * Q.Z;
        return Result;
    }

    FQuat Inverse() const
    {
        FQuat Result;
        Result.X = -X;
        Result.Y = -Y;
        Result.Z = -Z;
        Result.W = W;
        return Result;
    }

    void Normalize(float Tolerance = 1.e-8f)
    {
        const float SquareSum = X * X + Y * Y + Z * Z + W * W;
        if (SquareSum >= Tolerance)
        {
            const float Scale = 1.0f / FMath::Sqrt(SquareSum);
            X *= Scale;
            Y *= Scale;
            Z *= Scale;
            W *= Scale;
        }
        else
        {
            *this = FQuat();
        }
    }
};

static_assert(sizeof(FQuat) == 0x10);
