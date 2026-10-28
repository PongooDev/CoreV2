#pragma once

#include "GameplayTagContainer.h"
#include "StructArray.h"
#include "UObject/Object.h"
#include "UObject/ScriptInterface.h"
#include "UObject/Stack.h"

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
    DefineUFunction(GetAIDirector)
    DefineUFunction(GetAIGoalManager)
    DefineUFunction(GiveItemToInventoryOwner)
    DefineUFunction(K2_GiveItemToPlayer)
    DefineUFunction(K2_GiveItemToAllPlayers)
    DefineUFunction(K2_RemoveItemFromPlayer)
    DefineUFunction(K2_RemoveItemFromAllPlayers)
    DefineUFunction(K2_SpawnPickupInWorld)
    DefineUFunction(PickLootDrops)

public:
    static void EquipFortAbilitySet(TScriptInterface<IAbilitySystemInterface> AbilitySystemInterfaceActor, UFortAbilitySet* AbilitySet, UObject* OverrideSourceObject, FFortAbilitySetHandle& OutHandle);
    static AFortAIDirector* GetAIDirector(UObject* WorldContextObject);
    static AFortAIGoalManager* GetAIGoalManager(UObject* WorldContextObject);

    // Level is the new items' level (the Blueprint functions give level 1); bLoadFullClip gives a ranged weapon a full
    // magazine, the way Battle Royale's loot comes.
    static void GiveItemToInventoryOwner(TScriptInterface<IFortInventoryOwnerInterface> InventoryOwner, UFortWorldItemDefinition* ItemDefinition, int32_t NumberToGive, bool bNotifyPlayer, int32_t Level = 1, bool bLoadFullClip = false);
    static void K2_GiveItemToPlayer(AFortPlayerController* PlayerController, UFortWorldItemDefinition* ItemDefinition, int32_t NumberToGive, bool bNotifyPlayer, int32_t Level = 1, bool bLoadFullClip = false);
    static void K2_GiveItemToAllPlayers(UObject* WorldContextObject, UFortWorldItemDefinition* ItemDefinition, int32_t NumberToGive, bool bNotifyPlayer);
    static int32_t K2_RemoveItemFromPlayer(AFortPlayerController* PlayerController, UFortWorldItemDefinition* ItemDefinition, int32_t AmountToRemove);
    static void K2_RemoveItemFromAllPlayers(UObject* WorldContextObject, UFortWorldItemDefinition* ItemDefinition, int32_t AmountToRemove);
    static AFortPickup* K2_SpawnPickupInWorld(UObject* WorldContextObject, UFortWorldItemDefinition* ItemDefinition, int32_t NumberToSpawn, const FVector& Position, const FVector& Direction, int32_t OverrideMaxStackCount, bool bToss = true);
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
};
