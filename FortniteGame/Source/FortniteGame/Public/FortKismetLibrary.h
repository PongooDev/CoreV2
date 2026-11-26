#pragma once

#include "Containers/Map.h"
#include "GameplayTagContainer.h"
#include "StructArray.h"
#include "UObject/Object.h"
#include "UObject/ScriptInterface.h"
#include "UObject/Stack.h"

class AActor;
class AFortAIDirector;
class AFortAIGoalManager;
class AFortPickup;
class AFortPlayerController;
struct FGuid;
struct FRotator;
struct FVector;
class IAbilitySystemInterface;
class UAbilitySystemComponent;
class UClass;
class IFortInventoryOwnerInterface;
class UFortItem;
class UFortWorldItemDefinition;
class UFortAbilitySet;
struct FFortAbilitySetHandle;
struct FFortItemEntry;

class UFortKismetLibrary : public UObject
{
    DefineUnrealClass(UFortKismetLibrary, FortniteGame)
    DefineUFunction(EquipFortAbilitySet)
    DefineUFunction(UnequipFortAbilitySet)
    DefineUFunction(GetAIDirector)
    DefineUFunction(GetAIGoalManager)
    DefineUFunction(GiveItemToInventoryOwner)
    DefineUFunction(K2_GiveItemToPlayer)
    DefineUFunction(K2_GiveItemToAllPlayers)
    DefineUFunction(K2_RemoveItemFromPlayer)
    DefineUFunction(K2_RemoveItemFromPlayerByGuid)
    DefineUFunction(K2_RemoveFortItemFromPlayer)
    DefineUFunction(K2_RemoveItemFromAllPlayers)
    DefineUFunction(K2_SpawnPickupInWorld)
    DefineUFunction(K2_SpawnPickupInWorldWithClass)
    DefineUFunction(GetSafeZoneLocation)
    DefineUFunction(PickLootDrops)
    DefineUFunction(PickLootDropsWithNamedWeights)
    DefineUFunction(GetActorTeam)
    DefineUFunction(GetTeamAffiliation)
    DefineUFunction(K2_GiveBuildingResource)
    DefineUFunction(MakeGameplayEffectContainerSpecFromAbilityComponent)
    DefineUFunction(SpawnProjectile)

public:
    // The game's team of Actor (3.5.1 0xC35E40): its IFortTeamActorInterface's GetTeam, else its Instigator's, else
    // EFortTeam_MAX. A trap's blueprint compares its own Team with the local controller's this way (Trap_Floor_Spikes_C's
    // ChangeToFriendlyMaterial). Reflection::InvalidEnumValue where the build has no GetActorTeam.
    static uint8_t GetActorTeam(const AActor* Actor);
    // The game's attitude of ActorA towards ActorB (EFortTeamAffiliation: Friendly 0, Neutral 1, Hostile 2; 6.20
    // 0xF651A0 compares each one's GetActorTeam through the game state's team attitudes): Monster is Hostile to the
    // HumanPvP teams. Reflection::InvalidEnumValue where the build has no GetTeamAffiliation.
    static uint8_t GetTeamAffiliation(const AActor* ActorA, const AActor* ActorB);
    static void EquipFortAbilitySet(TScriptInterface<IAbilitySystemInterface> AbilitySystemInterfaceActor, UFortAbilitySet* AbilitySet, UObject* OverrideSourceObject, FFortAbilitySetHandle& OutHandle);
    static AFortAIDirector* GetAIDirector(UObject* WorldContextObject);
    static AFortAIGoalManager* GetAIGoalManager(UObject* WorldContextObject);

    // Level is the new items' level (the Blueprint functions give level 1); bLoadFullClip gives a ranged weapon a full
    // magazine, the way Battle Royale's loot comes. bDropOverflow (Core's cheats, #188): what the backpack has no room
    // for drops at the player's feet, as a pickup's leftover does, instead of going in past the backpack's size.
    static void GiveItemToInventoryOwner(TScriptInterface<IFortInventoryOwnerInterface> InventoryOwner, UFortWorldItemDefinition* ItemDefinition, int32_t NumberToGive, bool bNotifyPlayer, int32_t Level = 1, bool bLoadFullClip = false, bool bDropOverflow = false);
    static void K2_GiveItemToPlayer(AFortPlayerController* PlayerController, UFortWorldItemDefinition* ItemDefinition, int32_t NumberToGive, bool bNotifyPlayer, int32_t Level = 1, bool bLoadFullClip = false, bool bDropOverflow = false);
    static void K2_GiveItemToAllPlayers(UObject* WorldContextObject, UFortWorldItemDefinition* ItemDefinition, int32_t NumberToGive, bool bNotifyPlayer);
    // bForceRemoval (3.2 added it): an item its definition keeps at 0 (3.3's Remote Explosives detonator) goes too.
    static int32_t K2_RemoveItemFromPlayer(AFortPlayerController* PlayerController, UFortWorldItemDefinition* ItemDefinition, int32_t AmountToRemove, bool bForceRemoval = false);
    // 5.40 added it: the one stack ItemGuid names, AmountToRemove of it (all of it when negative). Returns how many went.
    static int32_t K2_RemoveItemFromPlayerByGuid(AFortPlayerController* PlayerController, const FGuid& ItemGuid, int32_t AmountToRemove, bool bForceRemoval = false);
    // 7.30 added it: as K2_RemoveItemFromPlayerByGuid, for the stack Item is.
    static int32_t K2_RemoveFortItemFromPlayer(AFortPlayerController* PlayerController, UFortItem* Item, int32_t AmountToRemove, bool bForceRemoval = false);
    static void K2_RemoveItemFromAllPlayers(UObject* WorldContextObject, UFortWorldItemDefinition* ItemDefinition, int32_t AmountToRemove);
    // The building resource item of a resource type (the game data's wood, stone, metal or permanite item), or null.
    static UFortWorldItemDefinition* K2_GetResourceItemDefinition(uint8_t ResourceType);
    // ResourceAmount of a building resource (EFortResourceType) for Controller, as a harvest gives it.
    static void K2_GiveBuildingResource(AFortPlayerController* Controller, uint8_t ResourceType, int32_t ResourceAmount);
    // bToss from 1.8, bRandomRotation from 3.0, bBlockedFromAutoPickup from 5.30 (their exec reads each only where the
    // build's UFunction has it).
    // PickupInstigatorHandle from 9.10: the IFortPickupInstigator's tracked index in its UFortPickupInstigatorRegistryComponent
    // (INDEX_NONE for none), which the pickup's item entry carries.
    static AFortPickup* K2_SpawnPickupInWorld(UObject* WorldContextObject, UFortWorldItemDefinition* ItemDefinition, int32_t NumberToSpawn, const FVector& Position, const FVector& Direction, int32_t OverrideMaxStackCount, bool bToss = true, bool bRandomRotation = false, bool bBlockedFromAutoPickup = false, int32_t PickupInstigatorHandle = -1);
    // From 6.00: K2_SpawnPickupInWorld with the pickup's class (null: the game's own pickup class). 9.00's (CL-6337466)
    // Creative Item Spawner (Minigame_Spawner_Component's Spawn) spawns its items through it as FortPickupCreative.
    static AFortPickup* K2_SpawnPickupInWorldWithClass(UObject* WorldContextObject, UFortWorldItemDefinition* ItemDefinition, UClass* PickupClass, int32_t NumberToSpawn, const FVector& Position, const FVector& Direction, int32_t OverrideMaxStackCount, bool bToss = true, bool bRandomRotation = false, bool bBlockedFromAutoPickup = false, int32_t PickupInstigatorHandle = -1);
    static bool PickLootDrops(TStructArray<FFortItemEntry>& OutLootToDrop, FName TierGroupName, int32_t WorldLevel, int32_t ForcedLootTier);
    // The game's own GetSafeZoneLocation (6.30 0xFA82D0, in the build): the Battle Royale game mode's SafeZoneLocations
    // entry for SafeZoneIndex, the centre the storm's circle of that phase closes on. False outside Battle Royale, before
    // the storm has picked them, for an index it hasn't, and on builds without the function.
    static bool GetSafeZoneLocation(UObject* WorldContextObject, int32_t SafeZoneIndex, FVector& OutLocation);
    // 5.0 (UE 4.21) added WorldContextObject as the first parameter; its exec passes it to the native first.
    static bool PickLootDrops(UObject* WorldContextObject, TStructArray<FFortItemEntry>& OutLootToDrop, FName TierGroupName, int32_t WorldLevel, int32_t ForcedLootTier);
    // 7.x: PickLootDrops whose package rows NamedWeightsMap reweighs by their NamedWeightMult (7.10's exec, 0x15E0530,
    // passes rcx = WorldContextObject, rdx = OutLootToDrop, r8 = TierGroupName, r9d = WorldLevel, then NamedWeightsMap and
    // ForcedLootTier on the stack).
    static bool PickLootDropsWithNamedWeights(UObject* WorldContextObject, TStructArray<FFortItemEntry>& OutLootToDrop, FName TierGroupName, int32_t WorldLevel, const TMap<FName, float>& NamedWeightsMap, int32_t ForcedLootTier);
    static bool PickLootTierKeyAndLootTierFromTierGroup(FName& OutLootTierKey, int32_t& OutLootTier, FName TierGroupName, int32_t WorldLevel, int32_t ForcedLootTier, const FGameplayTagContainer& ContextTags);
    // EffectContainer (an FFortGameplayEffectContainer) made into OutSpec (an FFortGameplayEffectContainerSpec's storage)
    // for AbilityComp at GameplayEffectLevel, as an ability makes its own (1.11 0x6346C0 hands it to the container's
    // 0x2BE8F0): its target selection and cues, and a spec of each of its effects in a context of the ability system's.
    // False where the build has no such function.
    static bool MakeGameplayEffectContainerSpecFromAbilityComponent(const void* EffectContainer, UAbilitySystemComponent* AbilityComp, int32_t GameplayEffectLevel, void* OutSpec);
    // A projectile of ProjectileClass for RequestedBy, from SpawnLocation along SpawnDirection (1.11 0x63E6A0, which
    // AFortWeaponRanged::SpawnProjectile, 0x9C20A0, calls for a player's shot with the weapon as RequestedBy). Server
    // only: it spawns nothing unless RequestedBy has authority. The projectile's owner is RequestedBy and its Instigator
    // RequestedBy's; it keeps the two specs (FFortGameplayEffectContainerSpec, either may be null for none) to apply where
    // it hits and where it explodes. bUseDefaultPhysics: the projectile's own speed, else InitialSpeed. Null where the
    // build has no such function or nothing spawned.
    static AActor* SpawnProjectile(UClass* ProjectileClass, AActor* RequestedBy, const FVector& SpawnLocation, const FRotator& SpawnRotation, const FRotator& SpawnDirection,
        const void* EffectContainerSpecToApplyOnHit, const void* EffectContainerSpecToApplyOnExplode, bool bUseDefaultPhysics, AActor* HomingTarget, float InitialSpeed,
        float GravityScale, float ChargePercent);

    DECLARE_FUNCTION(execGetAIDirector);
    DECLARE_FUNCTION(execGetAIGoalManager);
    DECLARE_FUNCTION(execGiveItemToInventoryOwner);
    DECLARE_FUNCTION(execK2_GiveItemToPlayer);
    DECLARE_FUNCTION(execK2_GiveItemToAllPlayers);
    DECLARE_FUNCTION(execK2_RemoveItemFromPlayer);
    DECLARE_FUNCTION(execK2_RemoveItemFromPlayerByGuid);
    DECLARE_FUNCTION(execK2_RemoveFortItemFromPlayer);
    DECLARE_FUNCTION(execK2_RemoveItemFromAllPlayers);
    DECLARE_FUNCTION(execK2_SpawnPickupInWorld);
    DECLARE_FUNCTION(execK2_SpawnPickupInWorldWithClass);
    DECLARE_FUNCTION(execK2_GiveBuildingResource);

    static void HookNatives();

private:
    static bool HasForceRemoval();
    // 8.20 (CL-5625478) added GiveItemToInventoryOwner's ItemLevel.
    static bool HasItemLevel();
    // The folded native's callers (ABuildingItemCollectorActor's deposit): 3.2's native takes bForceRemoval, older ones
    // leave the register unset.
    static int32_t K2_RemoveItemFromPlayerHK(AFortPlayerController* PlayerController, UFortWorldItemDefinition* ItemDefinition, int32_t AmountToRemove, bool bForceRemoval);
    // The stripped native's callers (6.00's AFortPlayerControllerGameplay::StartGhostMode): the game's four parameters,
    // and from 8.20 ItemLevel (its StartGhostMode, 0x15B4F40, passes -1); older callers leave that stack slot unset.
    static void GiveItemToInventoryOwnerHK(TScriptInterface<IFortInventoryOwnerInterface> InventoryOwner, UFortWorldItemDefinition* ItemDefinition, int32_t NumberToGive, bool bNotifyPlayer, int32_t ItemLevel);
};
