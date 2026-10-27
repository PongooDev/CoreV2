#pragma once

#include "Components/ActorComponent.h"
#include "Containers/EnumAsByte.h"
#include "Math/Vector.h"
#include "Reflection.h"

namespace EComponentMobility
{
    enum Type
    {
        Static,
        Stationary,
        Movable,
    };
}

class USceneComponent : public UActorComponent
{
    DefineUnrealClass(USceneComponent, Engine)

public:
    DefineUProperty(TEnumAsByte<EComponentMobility::Type>, Mobility)
    DefineUProperty(USceneComponent*, AttachParent)
    DefineUProperty(FVector, RelativeLocation)
};
