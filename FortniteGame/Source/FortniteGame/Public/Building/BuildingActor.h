#pragma once

#include "FortTeamInfo.h"
#include "GameFramework/Actor.h"
#include "GameplayTagContainer.h"
#include "UObject/ScriptDelegates.h"

class UAbilitySystemComponent;

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
    DefineUFunction(GetCentroid)

public:
    DefineUProperty(int32_t, OwnerPersistentID)
    // Spectator (the constructor's, 1.7.2 0x40B548) until the building gets one. A piece's team decides who may edit it:
    // ABuildingSMActor's CanBePlayerEdited (1.7.2 0x461EF0, 1.8 0x4646F0) lets only the editor's own team edit it, anyone
    // when it is Spectator. A trap's targets are filtered by it: a pawn on its team is friendly (AFortGameState 0x583360),
    // any other an enemy; no team is friendly to everyone (see FFortTargetFilter).
    DefineUProperty(EFortTeam, Team)
    // UFortAbilitySystemComponent: a trap's holds the abilities its AbilitySet grants.
    DefineUProperty(UAbilitySystemComponent*, AbilitySystemComponent)
    DefineUProperty(FGameplayTagContainer, DefaultTags)
    DefineUProperty(FGameplayTagContainer, StaticGameplayTags)
    DefineBitfieldUProperty(bDestroyed)
    DefineBitfieldUProperty(bPersistToWorld)
    DefineUProperty(int32_t, CurrentBuildingLevel)
    // Set on props and trees a player's new piece replaces: the piece kills them as it is placed.
    DefineBitfieldUProperty(bDestroyOnPlayerBuildingPlacement)
    DefineUProperty(FMulticastScriptDelegate, OnDied)
    DefineBitfieldUProperty(bPlayerPlaced)
    DefineUProperty(bool, bFireBuiltAndDestroyedEvents)

    float GetHealth() const;
    // Whether the zone's structure count includes this piece: one alive, player built (IsPlayerBuilt, 1.8 0x47F850: an
    // OwnerPersistentID or bPlayerPlaced) and with bFireBuiltAndDestroyedEvents, which every ABuildingActor has by default
    // (1.8 0x458440), traps included. InitializeBuildingActor counts such a piece (1.8 0x47BED0) and OnDeathServer takes it
    // off (1.8 0x47AA10).
    bool CountsTowardStructureLimit() const;
    float GetMaxHealth() const;
    FVector GetCentroid() const;
};

class AFortPlacementActor : public ABuildingActor
{
    DefineUnrealClass(AFortPlacementActor, FortniteGame)
};
