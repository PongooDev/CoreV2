#pragma once

#include "GameFramework/Actor.h"
#include "GameplayTagContainer.h"
#include "UObject/ScriptDelegates.h"

class EFortBuildingPersistentState
{
    DefineUnrealEnum(EFortBuildingPersistentState, FortniteGame)
    DefineUEnumValue(Default)
    DefineUEnumValue(New)
    DefineUEnumValue(Searched)
};

class ABuildingActor : public AActor
{
    DefineUnrealClass(ABuildingActor, FortniteGame)
    DefineUFunction(GetHealth)
    DefineUFunction(GetMaxHealth)

public:
    DefineUProperty(int32_t, OwnerPersistentID)
    DefineUProperty(FGameplayTagContainer, DefaultTags)
    DefineUProperty(FGameplayTagContainer, StaticGameplayTags)
    DefineBitfieldUProperty(bDestroyed)
    DefineBitfieldUProperty(bPersistToWorld)
    DefineUProperty(FMulticastScriptDelegate, OnDied)

    float GetHealth() const;
    float GetMaxHealth() const;
};

class AFortPlacementActor : public ABuildingActor
{
    DefineUnrealClass(AFortPlacementActor, FortniteGame)
};
