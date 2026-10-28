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

class UFortAISpawnGroup : public UObject
{
    DefineUnrealClass(UFortAISpawnGroup, FortniteGame)

public:
    DefineUProperty(FGameplayTagContainer, SpawnGroupGameplayTags)
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

    // Queues the group's enemies with the AI director, at SpawnLocation (native, BlueprintCallable). False when the build
    // has no such function.
    bool SpawnAIGroup(const UFortAISpawnGroup* SpawnGroupToSpawn, const FVector& SpawnLocation, const FRotator& SpawnRotation, bool bAllowAssigningToExternalSpawners);

    void RollSpawnGroupUpgradeModifiers(const FGuid& GroupGuid, const FFortAISpawnGroupUpgrade& Upgrade, float DifficultyLevel, const FGameplayTagContainer& EnemyTags);

    static void HookCallSites();

private:
    static void TryUpgradeSpawnGroupCall(UFortAIEncounterInfo* This, const FGuid& GroupGuid, bool bForEnemySpawner);
    static int32_t GetSpawnGroupCostCall(UFortAISpawnGroup* SpawnGroup, const FFortAISpawnGroupUpgrade* Upgrade, float DifficultyLevel, int32_t NumPlayers);
    static void PickPawnUpgradeCall(const FFortAISpawnGroupUpgrade* Upgrade, const FGameplayTagContainer& EnemyTags, int32_t NumPlayers, void* OutPawnUpgrade);
};
