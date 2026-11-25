#pragma once

#include "Math/Matrix.h"
#include "Math/Quat.h"
#include "Math/Vector.h"

#include <cmath>

struct alignas(16) FTransform
{
    FQuat Rotation;
    FVector Translation;
    float TranslationW = 0.0f;
    FVector Scale3D = FVector(1.0f, 1.0f, 1.0f);
    float Scale3DW = 0.0f;

    FVector GetLocation() const { return Translation; }
    void SetLocation(const FVector& Origin) { Translation = Origin; }
    FVector TransformPosition(const FVector& V) const { return Rotation.RotateVector(V * Scale3D) + Translation; }
    FVector InverseTransformPosition(const FVector& V) const { return Rotation.UnrotateVector(V - Translation) * GetSafeScaleReciprocal(Scale3D); }
    FVector InverseTransformVectorNoScale(const FVector& V) const { return Rotation.UnrotateVector(V); }

    static FVector GetSafeScaleReciprocal(const FVector& InScale, float Tolerance = 1.e-8f)
    {
        const auto Reciprocal = [Tolerance](float Value) { return Value <= Tolerance && Value >= -Tolerance ? 0.0f : 1.0f / Value; };
        return FVector(Reciprocal(InScale.X), Reciprocal(InScale.Y), Reciprocal(InScale.Z));
    }

    FMatrix ToMatrixWithScale() const
    {
        FMatrix Out;
        Out.M[3][0] = Translation.X;
        Out.M[3][1] = Translation.Y;
        Out.M[3][2] = Translation.Z;
        const float X2 = Rotation.X + Rotation.X;
        const float Y2 = Rotation.Y + Rotation.Y;
        const float Z2 = Rotation.Z + Rotation.Z;
        const float XX2 = Rotation.X * X2, YY2 = Rotation.Y * Y2, ZZ2 = Rotation.Z * Z2;
        const float YZ2 = Rotation.Y * Z2, WX2 = Rotation.W * X2;
        const float XY2 = Rotation.X * Y2, WZ2 = Rotation.W * Z2;
        const float XZ2 = Rotation.X * Z2, WY2 = Rotation.W * Y2;
        Out.M[0][0] = (1.0f - (YY2 + ZZ2)) * Scale3D.X;
        Out.M[1][1] = (1.0f - (XX2 + ZZ2)) * Scale3D.Y;
        Out.M[2][2] = (1.0f - (XX2 + YY2)) * Scale3D.Z;
        Out.M[2][1] = (YZ2 - WX2) * Scale3D.Z;
        Out.M[1][2] = (YZ2 + WX2) * Scale3D.Y;
        Out.M[1][0] = (XY2 - WZ2) * Scale3D.Y;
        Out.M[0][1] = (XY2 + WZ2) * Scale3D.X;
        Out.M[2][0] = (XZ2 + WY2) * Scale3D.Z;
        Out.M[0][2] = (XZ2 - WY2) * Scale3D.X;
        Out.M[3][3] = 1.0f;
        return Out;
    }

    // Epic's (TransformVectorized.h): identity when no axis has scale.
    FTransform Inverse() const
    {
        if (std::fabs(Scale3D.X) <= 1.e-8f && std::fabs(Scale3D.Y) <= 1.e-8f && std::fabs(Scale3D.Z) <= 1.e-8f)
        {
            return FTransform();
        }
        FTransform Result;
        Result.Rotation = Rotation.Inverse();
        Result.Scale3D = GetSafeScaleReciprocal(Scale3D);
        Result.Translation = Result.Rotation.RotateVector(Result.Scale3D * Translation) * -1.0f;
        return Result;
    }

    // Epic's FTransform::Multiply: A, then B (A * B).
    friend FTransform operator*(const FTransform& A, const FTransform& B)
    {
        FTransform Result;
        if (AnyHasNegativeScale(A.Scale3D, B.Scale3D))
        {
            ConstructTransformFromMatrixWithDesiredScale(A.ToMatrixWithScale(), B.ToMatrixWithScale(), A.Scale3D * B.Scale3D, Result);
            return Result;
        }
        Result.Rotation = B.Rotation * A.Rotation;
        Result.Scale3D = A.Scale3D * B.Scale3D;
        Result.Translation = B.Rotation.RotateVector(B.Scale3D * A.Translation) + B.Translation;
        return Result;
    }

private:
    static bool AnyHasNegativeScale(const FVector& A, const FVector& B)
    {
        return std::fmin(A.X, B.X) < 0.0f || std::fmin(A.Y, B.Y) < 0.0f || std::fmin(A.Z, B.Z) < 0.0f;
    }

    static void ConstructTransformFromMatrixWithDesiredScale(const FMatrix& AMatrix, const FMatrix& BMatrix, const FVector& DesiredScale, FTransform& OutTransform)
    {
        FMatrix M = AMatrix * BMatrix;
        M.RemoveScaling();
        const auto Sign = [](float Value) { return Value >= 0.0f ? 1.0f : -1.0f; };
        M.SetAxis(0, M.GetScaledAxis(0) * Sign(DesiredScale.X));
        M.SetAxis(1, M.GetScaledAxis(1) * Sign(DesiredScale.Y));
        M.SetAxis(2, M.GetScaledAxis(2) * Sign(DesiredScale.Z));
        FQuat Rotation = QuatFromMatrix(M);
        Rotation.Normalize();
        OutTransform.Scale3D = DesiredScale;
        OutTransform.Rotation = Rotation;
        OutTransform.Translation = M.GetOrigin();
    }
};

static_assert(sizeof(FTransform) == 0x30);
