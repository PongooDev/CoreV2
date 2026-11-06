#pragma once

#include "Abilities/FortPlayerAttrSet.h"
#include "Containers/Array.h"
#include "Engine/DataTable.h"
#include "GameFramework/Pawn.h"
#include "GameplayTagContainer.h"
#include "StructArray.h"
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

// Who damaged a pawn and how much (AFortPawn::Damagers, transient, newest last).
struct FDamagerInfo
{
    DefineUnrealStruct(FDamagerInfo, FortniteGame)
    DefineUProperty(AActor*, DamageCauser)
    DefineUProperty(int32_t, DamageAmount)
};

class AFortPawn : public APawn
{
    DefineUnrealClass(AFortPawn, FortniteGame)
    DefineUFunction(PawnStartFire)
    DefineUFunction(PawnStopFire)
    DefineUFunction(ForceKill)
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
    static bool HasCurrentWeapon() { return Member_CurrentWeapon.IsAvailable(); }
    DefineUProperty(TArray<AFortWeapon*>, CurrentWeaponList)
    DefineUProperty(FMulticastScriptDelegate, OnDied)
    DefineUProperty(FName, SpottedEvent)
    // Down But Not Out: knocked down until a teammate revives the pawn or it bleeds out.
    DefineUProperty(bool, bIsDBNO)
    DefineUProperty(bool, bPlayedDying)
    // Set on the server as the pawn dies (replicated: HandleIsDying).
    DefineUProperty(bool, bIsDying)
    // EFortMovementStyle: Running 0, Walking 1, Charging 2, Sprinting 3 (replicated).
    DefineUProperty(uint8_t, CurrentMovementStyle)
    // The damage tags of the hit that killed the pawn.
    DefineUProperty(FGameplayTagContainer, DeathTags)
    // The pawn's own tags (a husk's NPC.CharacterType.Husk.Basic and the like, from its class defaults).
    DefineUProperty(FGameplayTagContainer, GameplayTags)
    DefineUStructArrayProperty(FDamagerInfo, Damagers)
    DefineUProperty(UFortHealthSet*, HealthSet)
    // What the pawn's IAbilitySystemInterface returns (its player state's, for a player).
    DefineUProperty(UAbilitySystemComponent*, AbilitySystemComponent)
    // The pawn's FFortPawnStats row. Not replicated: each machine reads its own copy.
    DefineUStructProperty(FDataTableRowHandle, PawnStatHandle)
    // Its player state's movement attributes, for a player (replicated with the player state).
    DefineUProperty(UFortMovementSet*, MovementSet)
    // The god cheat's flag (UFortCheatManager::God, 3.3 0xE58C40, toggles it on the possessed pawn): the pawn's damage
    // check (3.3 0xE6AB30) makes every hit 0 while it, or bCanBeDamaged off, says so.
    DefineBitfieldUProperty(bIsInvulnerable)
    static bool HasInvulnerable() { return Member_bIsInvulnerable.IsAvailable(); }

    bool IsDead() const;
    // Server: the current weapon starts or stops firing in FireModeNum, as a player's held fire button does.
    void PawnStartFire(uint8_t FireModeNum = 0);
    void PawnStopFire(uint8_t FireModeNum = 0);
    // Server: the pawn dies at once, KillerController's elimination (its death runs as any other: the controller's
    // OnPawnDied with KillerController as the instigator), DeathReason its damage tag.
    void ForceKill(const FGameplayTag& DeathReason, AController* KillerController, AActor* KillerActor);
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
    // Server (#155): MaxHealth's current value made NewMaxHealth whatever its effects say (Battle Royale's
    // GE_Map_Athena_Vitality_Health overrides it to 100, which no base value gets past). The value is the property's
    // until the effects on MaxHealth or its base change again. Health's clamp follows it, so health can reach it, and a
    // health above it comes down to it. False without a health set.
    bool ForceMaxHealth(float NewMaxHealth);
    // Server (#155): the health set's maximum shield (Shield) made NewMaxShield whatever its effects say, the way
    // ForceMaxHealth does it (Battle Royale's GE_Map_Athena_Vitality_Health caps it at 100), and the shield's clamp with
    // it. False without a health set that has shields.
    bool ForceMaxShield(float NewMaxShield);
    // Server (#155): the shield (CurrentShield, base and current value), clamped to the maximum shield; it replicates
    // with the health set. False without a health set that has shields.
    bool SetShield(float NewShield);

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

    // Server, before each frame's replication (UNetDriver::AddPreReplicationCallback): the null entries of the
    // connections' pawns' CurrentWeaponList, left by destroyed weapons, are taken out; 3.4's replication graph reads
    // every entry.
    static void RemoveDestroyedWeaponsFromLists();
    static ptrdiff_t CurrentWeaponListOffset();
    static ptrdiff_t IsDBNOOffset();
};
