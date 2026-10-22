#pragma once

#include "Natives.h"
#include "ObjectLayout.h"
#include "UObject/Object.h"

class AFortPlayerController;

class UFortSpottableActorInterface : public UObject
{
    DefineUnrealClass(UFortSpottableActorInterface, FortniteGame)
};

class IFortSpottableActorInterface
{
public:
    bool CanBeSpottedBy(const AFortPlayerController* PlayerController)
    {
        return CallVirtual<bool>(this, Natives::IFortSpottableActorInterface_CanBeSpottedByIndex, PlayerController);
    }
};
