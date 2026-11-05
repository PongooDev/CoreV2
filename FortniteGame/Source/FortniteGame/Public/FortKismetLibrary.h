#pragma once

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
struct FVector;
class IAbilitySystemInterface;
class IFortInventoryOwnerInterface;
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
    DefineUFunction(K2_RemoveItemFromAllPlayers)
    DefineUFunction(K2_SpawnPickupInWorld)
    DefineUFunction(PickLootDrops)
    DefineUFunction(GetActorTeam)

public:
    // The game's team of Actor (3.5.1 0xC35E40): its IFortTeamActorInterface's GetTeam, else its Instigator's, else
    // EFortTeam_MAX. A trap's blueprint compares its own Team with the local controller's this way (Trap_Floor_Spikes_C's
    // ChangeToFriendlyMaterial). Reflection::InvalidEnumValue where the build has no GetActorTeam.
    static uint8_t GetActorTeam(const AActor* Actor);
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
    static void K2_RemoveItemFromAllPlayers(UObject* WorldContextObject, UFortWorldItemDefinition* ItemDefinition, int32_t AmountToRemove);
    static AFortPickup* K2_SpawnPickupInWorld(UObject* WorldContextObject, UFortWorldItemDefinition* ItemDefinition, int32_t NumberToSpawn, const FVector& Position, const FVector& Direction, int32_t OverrideMaxStackCount, bool bToss = true, bool bRandomRotation = false);
    static bool PickLootDrops(TStructArray<FFortItemEntry>& OutLootToDrop, FName TierGroupName, int32_t WorldLevel, int32_t ForcedLootTier);
    static bool PickLootTierKeyAndLootTierFromTierGroup(FName& OutLootTierKey, int32_t& OutLootTier, FName TierGroupName, int32_t WorldLevel, int32_t ForcedLootTier, const FGameplayTagContainer& ContextTags);

    DECLARE_FUNCTION(execGetAIDirector);
    DECLARE_FUNCTION(execGetAIGoalManager);
    DECLARE_FUNCTION(execGiveItemToInventoryOwner);
    DECLARE_FUNCTION(execK2_GiveItemToPlayer);
    DECLARE_FUNCTION(execK2_GiveItemToAllPlayers);
    DECLARE_FUNCTION(execK2_RemoveItemFromPlayer);
    DECLARE_FUNCTION(execK2_RemoveItemFromAllPlayers);
    DECLARE_FUNCTION(execK2_SpawnPickupInWorld);

    static void HookNatives();

private:
    static bool HasForceRemoval();
    // The folded native's callers (ABuildingItemCollectorActor's deposit): 3.2's native takes bForceRemoval, older ones
    // leave the register unset.
    static int32_t K2_RemoveItemFromPlayerHK(AFortPlayerController* PlayerController, UFortWorldItemDefinition* ItemDefinition, int32_t AmountToRemove, bool bForceRemoval);
};
