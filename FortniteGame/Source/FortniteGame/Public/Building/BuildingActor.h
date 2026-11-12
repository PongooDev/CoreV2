#pragma once

#include "FortTeamInfo.h"
#include "GameFramework/Actor.h"
#include "GameplayTagContainer.h"
#include "UObject/ScriptDelegates.h"

class UAbilitySystemComponent;
class UFortBuildingActorSet;

class EFortBuildingPersistentState
{
    DefineUnrealEnum(EFortBuildingPersistentState, FortniteGame)
    DefineUEnumValue(Default)
    DefineUEnumValue(New)
    DefineUEnumValue(Searched)
};

// Why a building actor is initialized: InitializeBuildingActor and PostInitializeSpawnedBuildingActor take it. A UENUM in a
// namespace, so its native type is an int: 4.2's InitializeBuildingActor (0xAEB110) compares all of edx (mov r14d, edx;
// lea eax, [r14-1]; cmp eax, 1), and a bool passed there leaves edx's upper bytes to chance. When they aren't zero the
// piece skips the Spawned path: no Gameplay.Effect.Building.Create event, no ability system or BuildingAttributeSet, no
// build-up, so it stands at full health (#300). The values are the 3.0 to 4.2 SDKs' (Spawned was already 1 before).
namespace EFortBuildingInitializationReason
{
    enum Type : int32_t
    {
        StaticallyPlaced = 0,
        Spawned = 1,
        Replaced = 2,
        LoadedFromSave = 3,
        DynamicBuilderPlaced = 4,
        PlacementTool = 5,
        TrapTool = 6,
        None = 7,
    };
}

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
    // 2.5 (CL-3889387) dropped DefaultTags, leaving ConstTags and StaticGameplayTags.
    DefineUProperty(FGameplayTagContainer, DefaultTags)
    DefineUProperty(FGameplayTagContainer, StaticGameplayTags)
    bool HasDefaultTags() const { return Member_DefaultTags.IsAvailable(); }
    bool HasDefaultTag(const FGameplayTag& Tag) { return HasDefaultTags() && static_cast<FGameplayTagContainer&>(DefaultTags).HasTag(Tag); }
    DefineBitfieldUProperty(bDestroyed)
    DefineBitfieldUProperty(bPersistToWorld)
    DefineUProperty(int32_t, CurrentBuildingLevel)
    // Set on props and trees a player's new piece replaces: the piece kills them as it is placed.
    DefineBitfieldUProperty(bDestroyOnPlayerBuildingPlacement)
    DefineUProperty(FMulticastScriptDelegate, OnDied)
    DefineBitfieldUProperty(bPlayerPlaced)
    DefineBitfieldUProperty(bFireBuiltAndDestroyedEvents)
    // The piece's UFortBuildingActorSet (its BuildTime and RepairTime), made with its ability system.
    DefineUProperty(UFortBuildingActorSet*, BuildingAttributeSet)
    static bool HasBuildingAttributeSet() { return Member_BuildingAttributeSet.IsAvailable(); }
    // Replicated. TakeDamage (4.2 0xABF040) does nothing to an invulnerable building; MakeBuildingActorsGod flips it.
    DefineBitfieldUProperty(bIsInvulnerable)
    static bool HasInvulnerable() { return Member_bIsInvulnerable.IsAvailable(); }

    float GetHealth() const;
    // Whether the zone's structure count includes this piece: one alive, player built (IsPlayerBuilt, 1.8 0x47F850: an
    // OwnerPersistentID or bPlayerPlaced) and with bFireBuiltAndDestroyedEvents, which every ABuildingActor has by default
    // (1.8 0x458440), traps included. InitializeBuildingActor counts such a piece (1.8 0x47BED0) and OnDeathServer takes it
    // off (1.8 0x47AA10).
    bool CountsTowardStructureLimit() const;
    float GetMaxHealth() const;
    FVector GetCentroid() const;

    void PostInitializeComponents();
    static void HookVTables();

private:
    static inline void (*PostInitializeComponentsOG)(ABuildingActor* This) = nullptr;
    static void PostInitializeComponentsHK(ABuildingActor* This);
};

class AFortPlacementActor : public ABuildingActor
{
    DefineUnrealClass(AFortPlacementActor, FortniteGame)
};
