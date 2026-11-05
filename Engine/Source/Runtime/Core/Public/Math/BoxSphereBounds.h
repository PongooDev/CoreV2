#pragma once

#include "Math/Box.h"
#include "Math/Vector.h"

// A box and the sphere around it, centred on the same point (Engine's FBoxSphereBounds, 0x1C): a scene component's
// Bounds.
struct FBoxSphereBounds
{
    FVector Origin;
    FVector BoxExtent;
    float SphereRadius = 0.0f;

    FBoxSphereBounds() = default;

    explicit FBoxSphereBounds(const FBox& Box)
        : Origin(Box.GetCenter()), BoxExtent(Box.GetExtent()), SphereRadius(Box.GetExtent().Size())
    {
    }
};

static_assert(sizeof(FBoxSphereBounds) == 0x1C);
