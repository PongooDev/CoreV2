#pragma once

#include "GameFramework/Actor.h"
#include "Reflection.h"

class UModel;
class UPrimitiveComponent;

// An actor shaped by a BSP brush: Brush is its geometry (in the actor's space), BrushComponent the component its
// collision comes from (its BrushBodySetup is built from Brush when the level is saved) and its root.
class ABrush : public AActor
{
    DefineUnrealClass(ABrush, Engine)

public:
    DefineUProperty(UModel*, Brush)
    DefineUProperty(UPrimitiveComponent*, BrushComponent)

    UPrimitiveComponent* GetBrushComponent() const { return BrushComponent; }
};

class AVolume : public ABrush
{
    DefineUnrealClass(AVolume, Engine)
};
