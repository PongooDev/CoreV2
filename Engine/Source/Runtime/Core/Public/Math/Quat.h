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
};

static_assert(sizeof(FQuat) == 0x10);
