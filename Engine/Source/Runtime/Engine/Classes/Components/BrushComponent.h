#pragma once

#include "Components/PrimitiveComponent.h"
#include "Reflection.h"

class UBodySetup;

// A brush actor's component: its collision is BrushBodySetup (built from the brush when the level is saved).
class UBrushComponent : public UPrimitiveComponent
{
    DefineUnrealClass(UBrushComponent, Engine)

public:
    DefineUProperty(UBodySetup*, BrushBodySetup)
};
