#pragma once

#include "GameFramework/Actor.h"
#include "UObject/Object.h"

namespace EEndPlayReason
{
    enum Type
    {
        Destroyed,
        LevelTransition,
        EndPlayInEditor,
        RemovedFromWorld,
        Quit,
    };
}

class UActorComponent : public UObject
{
    DefineUnrealClass(UActorComponent, Engine)

public:
    AActor* GetOwner() const
    {
        for (UObject* Outer = GetOuter(); Outer; Outer = Outer->GetOuter())
        {
            if (Outer->IsA<AActor>())
            {
                return static_cast<AActor*>(Outer);
            }
        }
        return nullptr;
    }
};
