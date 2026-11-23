#pragma once

#include "Math/Quat.h"
#include "Math/Vector.h"

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
};

static_assert(sizeof(FTransform) == 0x30);
