#pragma once

#include "GameFramework/Actor.h"
#include "GameplayTagContainer.h"
#include "UObject/ScriptDelegates.h"

class ABuildingActor : public AActor
{
    DefineUnrealClass(ABuildingActor, FortniteGame)

public:
    DefineUProperty(FGameplayTagContainer, DefaultTags)
    DefineUProperty(FGameplayTagContainer, StaticGameplayTags)
    DefineUProperty(FMulticastScriptDelegate, OnDied)
};

class AFortPlacementActor : public ABuildingActor
{
    DefineUnrealClass(AFortPlacementActor, FortniteGame)
};
