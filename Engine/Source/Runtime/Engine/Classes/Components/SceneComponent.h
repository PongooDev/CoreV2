#pragma once

#include "Components/ActorComponent.h"
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
    DefineUFunction(SetMobility)

public:
    void SetMobility(EComponentMobility::Type NewMobility);
};
