#pragma once

#include "Math/Quat.h"
#include "Math/Vector.h"

#include <cmath>

// Epic's FMatrix (row vectors: a point is transformed as P * M, the origin in row 3). Only what FTransform's negative-scale
// paths and FBox::TransformBy need.
struct alignas(16) FMatrix
{
    float M[4][4] = {};

    FMatrix operator*(const FMatrix& Other) const
    {
        FMatrix Result;
        for (int32_t Row = 0; Row < 4; ++Row)
        {
            for (int32_t Column = 0; Column < 4; ++Column)
            {
                Result.M[Row][Column] = M[Row][0] * Other.M[0][Column] + M[Row][1] * Other.M[1][Column] + M[Row][2] * Other.M[2][Column] + M[Row][3] * Other.M[3][Column];
            }
        }
        return Result;
    }

    FVector TransformPosition(const FVector& V) const
    {
        return FVector(V.X * M[0][0] + V.Y * M[1][0] + V.Z * M[2][0] + M[3][0], V.X * M[0][1] + V.Y * M[1][1] + V.Z * M[2][1] + M[3][1],
            V.X * M[0][2] + V.Y * M[1][2] + V.Z * M[2][2] + M[3][2]);
    }

    FVector GetScaledAxis(int32_t Axis) const { return FVector(M[Axis][0], M[Axis][1], M[Axis][2]); }

    void SetAxis(int32_t Axis, const FVector& V)
    {
        M[Axis][0] = V.X;
        M[Axis][1] = V.Y;
        M[Axis][2] = V.Z;
    }

    FVector GetOrigin() const { return FVector(M[3][0], M[3][1], M[3][2]); }

    void RemoveScaling(float Tolerance = 1.e-8f)
    {
        for (int32_t Axis = 0; Axis < 3; ++Axis)
        {
            const float SquareSum = M[Axis][0] * M[Axis][0] + M[Axis][1] * M[Axis][1] + M[Axis][2] * M[Axis][2];
            const float Scale = SquareSum - Tolerance >= 0.0f ? 1.0f / std::sqrt(SquareSum) : 1.0f;
            M[Axis][0] *= Scale;
            M[Axis][1] *= Scale;
            M[Axis][2] *= Scale;
        }
    }
};

static_assert(sizeof(FMatrix) == 0x40);

// Epic's FQuat(const FMatrix&): the rotation of a matrix without scale (identity when an axis has none).
inline FQuat QuatFromMatrix(const FMatrix& M)
{
    const auto IsNearlyZero = [](const FVector& V) { return std::fabs(V.X) <= 1.e-4f && std::fabs(V.Y) <= 1.e-4f && std::fabs(V.Z) <= 1.e-4f; };
    FQuat Result;
    if (IsNearlyZero(M.GetScaledAxis(0)) || IsNearlyZero(M.GetScaledAxis(1)) || IsNearlyZero(M.GetScaledAxis(2)))
    {
        return Result;
    }

    const float Trace = M.M[0][0] + M.M[1][1] + M.M[2][2];
    if (Trace > 0.0f)
    {
        const float InvS = 1.0f / std::sqrt(Trace + 1.0f);
        const float S = 0.5f * InvS;
        Result.W = 0.5f * (1.0f / InvS);
        Result.X = (M.M[1][2] - M.M[2][1]) * S;
        Result.Y = (M.M[2][0] - M.M[0][2]) * S;
        Result.Z = (M.M[0][1] - M.M[1][0]) * S;
        return Result;
    }

    int32_t I = 0;
    if (M.M[1][1] > M.M[0][0])
    {
        I = 1;
    }
    if (M.M[2][2] > M.M[I][I])
    {
        I = 2;
    }
    static constexpr int32_t Next[3] = { 1, 2, 0 };
    const int32_t J = Next[I];
    const int32_t K = Next[J];
    float S = M.M[I][I] - M.M[J][J] - M.M[K][K] + 1.0f;
    const float InvS = 1.0f / std::sqrt(S);
    float Q[4];
    Q[I] = 0.5f * (1.0f / InvS);
    S = 0.5f * InvS;
    Q[3] = (M.M[J][K] - M.M[K][J]) * S;
    Q[J] = (M.M[I][J] + M.M[J][I]) * S;
    Q[K] = (M.M[I][K] + M.M[K][I]) * S;
    Result.X = Q[0];
    Result.Y = Q[1];
    Result.Z = Q[2];
    Result.W = Q[3];
    return Result;
}
