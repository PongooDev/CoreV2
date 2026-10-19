#pragma once

#include "GameFramework/Actor.h"
#include "GameplayTagContainer.h"

class ABuildingActor : public AActor
{
    DefineUnrealClass(ABuildingActor, FortniteGame)

public:
    DefineUProperty(FGameplayTagContainer, StaticGameplayTags)
};
