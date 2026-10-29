#pragma once

#include "Abilities/GameplayAbility.h"
#include "StructArray.h"
#include "UObject/Stack.h"
#include "UObject/WeakObjectPtr.h"

#include <cstdint>

class AFortPawn;
class UFortWeaponItemDefinition;
class UFortWorldItem;
class UTexture2D;

// Which actors a trap's trigger or an ability's target selection keeps (1.8.2 0x2ACB30 tests one actor). A pawn is a
// friend when it is on the requester's team (AFortGameState's check, 1.7.2 0x583360, 1.8.2 0x5C5A50: the two
// FortTeamActorInterface teams are equal; a trap's is ABuildingActor::Team) and an enemy otherwise.
struct FFortTargetFilter
{
    DefineUnrealStruct(FFortTargetFilter, FortniteGame)

public:
    DefineUProperty(bool, bExcludePawnFriends)
    DefineUProperty(bool, bExcludePawnEnemies)
};

struct FFortAbilityTargetSelection
{
    DefineUnrealStruct(FFortAbilityTargetSelection, FortniteGame)

public:
    DefineUStructProperty(FFortTargetFilter, TargetFilter)
};

struct FFortAbilityTargetSelectionList
{
    DefineUnrealStruct(FFortAbilityTargetSelectionList, FortniteGame)

public:
    DefineUStructArrayProperty(FFortAbilityTargetSelection, List)
};

struct FFortGameplayEffectContainer
{
    DefineUnrealStruct(FFortGameplayEffectContainer, FortniteGame)

public:
    DefineUStructProperty(FFortAbilityTargetSelectionList, TargetSelection)
};

class UFortGameplayAbility : public UGameplayAbility
{
    DefineUnrealClass(UFortGameplayAbility, FortniteGame)
    DefineUFunction(EquipAbilityWeapon)
    DefineUFunction(GetActivatingPawn)

public:
    DefineUProperty(UTexture2D*, SmallPreviewImageOverride)
    // The first of a fixed array (EffectContainers[5]); an ability's targets come from the one whose ApplicationTag it
    // asks for (GetGameplayEffectContainer, 1.8.2 0x29B420).
    DefineUStructProperty(FFortGameplayEffectContainer, EffectContainers)

    // Lets every target selection in its effect containers keep enemy pawns; how many filters it changed.
    int32_t IncludeEnemyTargets();

    AFortPawn* GetActivatingPawn() const;
    void EquipAbilityWeapon(UFortWeaponItemDefinition* WeaponDefinition, int32_t ItemLevel);
    void RemoveAbilityWeapon();

    TWeakObjectPtr<UFortWorldItem>& GetAbilityGrantedWeaponItem() const;

    static void HookNatives();

    DECLARE_FUNCTION(execEquipAbilityWeapon);

private:
    static inline void (*EndAbilityCleanupOG)(UFortGameplayAbility* This) = nullptr;
    static void EndAbilityCleanupCall(UFortGameplayAbility* This);
};
