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
};

static_assert(sizeof(FTransform) == 0x30);
