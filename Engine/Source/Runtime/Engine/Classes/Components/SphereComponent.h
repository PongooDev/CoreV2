#pragma once

#include "Components/ShapeComponent.h"
#include "Reflection.h"

class USphereComponent : public UShapeComponent
{
    DefineUnrealClass(USphereComponent, Engine)
    DefineUFunction(SetSphereRadius)

public:
    // The sphere's new radius; bUpdateOverlaps has the actors already inside begin (or end) their overlaps at once.
    void SetSphereRadius(float InSphereRadius, bool bUpdateOverlaps = true);
};
