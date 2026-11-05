#pragma once

#include "Components/ActorComponent.h"
#include "Containers/EnumAsByte.h"
#include "Math/BoxSphereBounds.h"
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

    // The component's world space bounds. Not reflected: SceneComponent.h declares it right before RelativeLocation
    // (3.5.1: Bounds +0x140, RelativeLocation +0x15C).
    FBoxSphereBounds& GetBounds()
    {
        return *reinterpret_cast<FBoxSphereBounds*>(reinterpret_cast<uint8_t*>(&GetMember_RelativeLocation()) - sizeof(FBoxSphereBounds));
    }
};
