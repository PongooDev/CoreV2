#pragma once

#include "Abilities/FortPlayerAttrSet.h"
#include "Containers/Array.h"
#include "Engine/DataTable.h"
#include "GameFramework/Pawn.h"
#include "Misc/Guid.h"
#include "UObject/ScriptDelegates.h"

class AFortWeapon;
class UAbilitySystemComponent;
class UFortWeaponItemDefinition;

// A row of a pawn's stats table (AFortPawn::PawnStatHandle). Battle Royale's pawn moves at its speeds
// (AFortPlayerPawnAthena::GetMovementSpeed, 1.7.2 0x41E9D0, 1.8 0x428410, 1.8.1 0x41EF10), flying included.
struct FFortPawnStats
{
    DefineUnrealStruct(FFortPawnStats, FortniteGame)
    DefineUProperty(float, SpeedWalk)
    DefineUProperty(float, SpeedRun)
    DefineUProperty(float, SpeedSprint)

    // Every speed the row moves a pawn at, times Scale (the crouched ones from 1.7.2).
    void ScaleSpeeds(float Scale);

private:
    DefineUProperty(float, SpeedCrouchedRun)
    DefineUProperty(float, SpeedCrouchedSprint)
};

class AFortPawn : public APawn
{
    DefineUnrealClass(AFortPawn, FortniteGame)
    DefineUFunction(IsDead)
    DefineUFunction(EquipWeaponDefinition)
    DefineUFunction(TriggerFeedbackEvent)
    DefineUFunction(GetHealth)
    DefineUFunction(GetMaxHealth)
    DefineUFunction(GetShield)
    DefineUFunction(GetMaxShield)
    DefineUFunction(SetHealth)
    DefineUFunction(SetMaxHealth)

public:
    DefineUProperty(AFortWeapon*, CurrentWeapon)
    DefineUProperty(TArray<AFortWeapon*>, CurrentWeaponList)
    DefineUProperty(FMulticastScriptDelegate, OnDied)
    DefineUProperty(FName, SpottedEvent)
    // Down But Not Out: knocked down until a teammate revives the pawn or it bleeds out.
    DefineUProperty(bool, bIsDBNO)
    DefineUProperty(bool, bPlayedDying)
    DefineUProperty(UFortHealthSet*, HealthSet)
    // What the pawn's IAbilitySystemInterface returns (its player state's, for a player).
    DefineUProperty(UAbilitySystemComponent*, AbilitySystemComponent)
    // The pawn's FFortPawnStats row. Not replicated: each machine reads its own copy.
    DefineUStructProperty(FDataTableRowHandle, PawnStatHandle)
    // Its player state's movement attributes, for a player (replicated with the player state).
    DefineUProperty(UFortMovementSet*, MovementSet)

    bool IsDead() const;
    AFortWeapon* EquipWeaponDefinition(const UFortWeaponItemDefinition* WeaponData, const FGuid& ItemEntryGuid);
    bool EquipDefaultWeapon();
    void RemoveAbilityWeapon(AFortWeapon* Weapon);
    void TriggerFeedbackEvent(FName EventName, AFortPawn* InstigatorPawn, AFortPawn* Recipient, float OverriddenDelay);
    float GetHealth() const;
    float GetMaxHealth() const;
    float GetShield() const;
    float GetMaxShield() const;
    // Server: the health set's MaxHealth and Health base values, through the pawn's own setters (they replicate). False
    // when this build has no setter.
    bool SetHealth(float NewHealth);
    bool SetMaxHealth(float NewMaxHealth);

    // "cheat fly 3000" (#129): the pawn runs at Speed (and walks and sprints faster by as much), 0 gives it its own
    // speeds back. Character movement is predicted, so the server and the owning client both run this with the same
    // speed, or the server corrects the client. Returns the pawn's own running speed, 0 when it has none.
    float SetCheatMoveSpeed(float Speed);
    // "cheat speed" (#155): the pawn walks, runs and sprints Multiplier times as fast as it does on its own (1: its own),
    // also after a "cheat walk"; a flying speed ("fly 3000") goes first while it flies. Returns its own running speed.
    float SetCheatSpeedMultiplier(float Multiplier);
    float GetCheatSpeedMultiplier() const;
    // The pawn's speeds are its movement set's (a Save the World player's: replicated to its client with the player
    // state), not its stats row's (Battle Royale's, which each machine reads on its own).
    bool MovesWithMovementSet() const;

    static ptrdiff_t CurrentWeaponListOffset();
    static ptrdiff_t IsDBNOOffset();
};
