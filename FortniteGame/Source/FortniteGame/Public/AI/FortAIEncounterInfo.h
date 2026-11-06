#pragma once

#include "Containers/Array.h"
#include "GameplayTagContainer.h"
#include "Items/FortItemDefinition.h"
#include "Math/Rotator.h"
#include "Math/Vector.h"
#include "Misc/Guid.h"
#include "StructArray.h"
#include "UObject/NameTypes.h"
#include "UObject/Object.h"

class UFortGameplayModifierItemDefinition : public UFortItemDefinition
{
    DefineUnrealClass(UFortGameplayModifierItemDefinition, FortniteGame)
};

class AFortAIPawn;

struct FFortAIPawnVariantDefinition
{
    DefineUnrealStruct(FFortAIPawnVariantDefinition, FortniteGame)

public:
    DefineUProperty(UClass*, PawnClass)
};

class UFortAIPawnVariant : public UObject
{
    DefineUnrealClass(UFortAIPawnVariant, FortniteGame)

public:
    DefineUStructArrayProperty(FFortAIPawnVariantDefinition, PawnVariantDefinitions)
};

struct FSpawnGroupEnemy
{
    DefineUnrealStruct(FSpawnGroupEnemy, FortniteGame)

public:
    // A UFortAIPawnVariant class.
    DefineUProperty(UClass*, EnemyVariantClass)
};

class UFortAISpawnGroup : public UObject
{
    DefineUnrealClass(UFortAISpawnGroup, FortniteGame)

public:
    DefineUStructArrayProperty(FSpawnGroupEnemy, EnemiesToSpawn)
    DefineUProperty(FGameplayTagContainer, SpawnGroupGameplayTags)

    // The tags of enemy EnemyIndex's pawn, added to OutTags (4.0 0x9028E0): the class defaults of the first pawn class
    // its variant lists. Nothing for an index out of range.
    void GetEnemyGameplayTags(int32_t EnemyIndex, FGameplayTagContainer& OutTags) const;
};

// 4.0: a row of /Game/AIDirector/BaseLootDrops, which the AI director's BaseLootDropData names. The row whose
// AITagsContainer the pawn's tags all have, of the highest priority, is what the pawn drops.
struct FFortAIBaseLootDropRow
{
    DefineUnrealStruct(FFortAIBaseLootDropRow, FortniteGame)

public:
    DefineUProperty(int32_t, Priority)
    DefineUProperty(float, WorldItemDropChance)
    DefineUProperty(FName, WorldItemLootTierGroup)
    DefineUProperty(float, AccountItemDropChance)
    DefineUProperty(FName, AccountItemLootTierGroup)
    DefineUProperty(FGameplayTagContainer, AITagsContainer)
};

// 4.0: a row of /Game/AIDirector/LootDropsModifiers: the chances of the encounter's pawns that have its tags, scaled.
struct FFortAILootDropModifierRow
{
    DefineUnrealStruct(FFortAILootDropModifierRow, FortniteGame)

public:
    DefineUProperty(int32_t, Priority)
    DefineUProperty(float, WorldItemDropChanceMultiplier)
    DefineUProperty(float, AccountItemDropChanceMultiplier)
    DefineUProperty(FGameplayTagContainer, AITagsContainer)
};

// 4.0's FFortAIPawnLootDropData (a pending spawn's LootDropData): what a pawn may drop when it dies.
struct FFortAIPawnLootDropData
{
    float WorldItemLootDropChance = 0.0f;
    FName WorldItemTierGroup;
    float AccountItemLootDropChance = 0.0f;
    FName AccountItemTierGroup;
};

struct FFortAISpawnGroupUpgrade
{
    DefineUnrealStruct(FFortAISpawnGroupUpgrade, FortniteGame)

public:
    DefineUProperty(FName, AdditionalModifiersLootTierGroup)
};

struct FSpawnGroupInstanceInfo
{
    DefineUnrealStruct(FSpawnGroupInstanceInfo, FortniteGame)

public:
    DefineUProperty(UFortAISpawnGroup*, SpawnGroup)
    DefineUProperty(FGuid, GroupGuid)
    // The upgrade's UpgradeTags (a Mini-Boss upgrade's Encounter.PopulationCategory.MiniBoss).
    DefineUProperty(FGameplayTagContainer, UpgradeTags)
    DefineUProperty(TArray<UFortItemDefinition*>, ModifiersForAllPawns)

private:
    friend class UFortAIEncounterInfo;
    DefineUProperty(int32_t, TotalGroupCost)
    DefineUProperty(FScriptArray, PawnUpgrades)
};

class UFortAIEncounterInfo : public UObject
{
    DefineUnrealClass(UFortAIEncounterInfo, FortniteGame)
    DefineUFunction(SpawnAIGroup)

public:
    DefineUProperty(TArray<UObject*>, EncounterAssignments)
    DefineUStructArrayProperty(FFortAISpawnGroupUpgrade, AvailableUpgrades)
    DefineUStructArrayProperty(FSpawnGroupInstanceInfo, ActiveSpawnGroups)
    // 4.0: the director's BaseLootDropData and the LootDropModifiers rows whose requirements the encounter meets, copied
    // in when it starts. Empty without UFortRuntimeOptions::bEnableSTWLootDrops.
    DefineUStructArrayProperty(FFortAIBaseLootDropRow, BaseLootDropRows)
    DefineUStructArrayProperty(FFortAILootDropModifierRow, LootDropModifierRows)
    static bool HasLootDropRows() { return Member_BaseLootDropRows.IsAvailable() && Member_LootDropModifierRows.IsAvailable(); }
    DefineUProperty(FGameplayTagContainer, ModifierTags)
    DefineUProperty(FGameplayTagContainer, GameContextTags)

    // Queues the group's enemies with the AI director, at SpawnLocation (native, BlueprintCallable). False when the build
    // has no such function.
    bool SpawnAIGroup(const UFortAISpawnGroup* SpawnGroupToSpawn, const FVector& SpawnLocation, const FRotator& SpawnRotation, bool bAllowAssigningToExternalSpawners);

    // 4.0 0x8BFD60: the base row and the modifier row (each the highest priority one whose AITagsContainer AITags has
    // all of) as the pawn's loot drop data, each chance the base row's times the modifier's, clamped to 0..1. Left as
    // it is when no base row matches.
    void GetAIPawnLootDropData(const FGameplayTagContainer& AITags, FFortAIPawnLootDropData& OutLootDropData) const;
    // The tags a pawn of this encounter is given its loot drop data for, as the encounter's spawn queue gathers them
    // (4.0 0x8DABE0): the encounter's, the group's upgrade tags and spawn group tags, and the enemy's own.
    void GetAIPawnLootDropTags(const AFortAIPawn* Pawn, FGameplayTagContainer& OutAITags) const;

    void RollSpawnGroupUpgradeModifiers(const FGuid& GroupGuid, const FFortAISpawnGroupUpgrade& Upgrade, float DifficultyLevel, const FGameplayTagContainer& EnemyTags);

    static void HookCallSites();

private:
    static void TryUpgradeSpawnGroupCall(UFortAIEncounterInfo* This, const FGuid& GroupGuid, bool bForEnemySpawner);
    static int32_t GetSpawnGroupCostCall(UFortAISpawnGroup* SpawnGroup, const FFortAISpawnGroupUpgrade* Upgrade, float DifficultyLevel, int32_t NumPlayers);
    static void PickPawnUpgradeCall(const FFortAISpawnGroupUpgrade* Upgrade, const FGameplayTagContainer& EnemyTags, int32_t NumPlayers, void* OutPawnUpgrade);
};
