#pragma once

#include "Building/BuildingActor.h"
#include "Items/FortLootPackage.h"
#include "StructArray.h"

class AFortPawn;
class AFortWorldManager;

class EFortResourceType
{
    DefineUnrealEnum(EFortResourceType, FortniteGame)
    DefineUEnumValue(Wood)
    DefineUEnumValue(Stone)
    DefineUEnumValue(Metal)
    DefineUEnumValue(Permanite)
    DefineUEnumValue(None)
};

struct FMeshSet
{
    DefineUnrealStruct(FMeshSet, FortniteGame)
};

struct FTierMeshSets
{
    DefineUnrealStruct(FTierMeshSets, FortniteGame)

public:
    DefineUProperty(int32_t, Tier)
    DefineUStructArrayProperty(FMeshSet, MeshSets)
};

// The world manager's pre-rolled lists (filled from its world generation stream), read round-robin, 0 when empty.
struct FFortBuildingRandomList
{
    float Next()
    {
        if (Num <= 0)
        {
            return 0.0f;
        }
        if (Index >= Num)
        {
            Index = 0;
        }
        return Values[Index++];
    }

    float* Values;
    int32_t ArrayNum;
    int32_t ArrayMax;
    int32_t Index;
    int32_t Num;
};

class ABuildingSMActor : public ABuildingActor
{
    DefineUnrealClass(ABuildingSMActor, FortniteGame)

public:
    DefineUStructArrayProperty(FTierMeshSets, AlternateMeshes)
    DefineUProperty(int32_t, AltMeshIdx)
    DefineUProperty(EFortResourceType, ResourceType)
    DefineUProperty(ELootQuotaLevel, MaximumQuotaLevelBound)
    DefineBitfieldUProperty(bCanSpawnAtLowerQuotaLevels)
    DefineUProperty(FName, DestructionLootTierGroup)
    DefineUStructProperty(FChosenQuotaInfo, DestructionLootTierChosenQuotaInfo)
    DefineUProperty(FName, DestructionLootTierKey)
    DefineUProperty(int32_t, MaxResourcesToSpawn)
    DefineBitfieldUProperty(bAllowResourceDrop)

    void PostUpdate(EFortBuildingPersistentState State, FFortBuildingRandomList* RandomList);
    void SelectMeshSet(int32_t LootTier);
    void SelectMeshSet(FName LootTierKey);
    void SetMeshSet(const FMeshSet& MeshSet);
    void AttemptSpawnResources(AFortPawn* InstigatorPawn, float Damage, bool bJustHitWeakspot);
    float DetermineMaxResourcesToSpawn(bool bRandomize);
    const FName& GetQuotaLootTierGroup();
    bool CanSpawnAtLowerQuotaLevels();
    ELootQuotaLevel GetMaximumQuotaLevelBound();
    void SetChosenQuotaInfo(const FName& LootTierKey, int32_t LootTier);
    void OnQuotaNotChosen();

    static bool ResolveLootQuota();
    static void HookVTables();

protected:
    AFortWorldManager* GetWorldManager() const;
    int32_t GetWorldLevel() const;
    void UpdateActorRecord(AFortWorldManager* WorldManager, EFortBuildingPersistentState State, bool bForce);

private:
    float& ResourceRemainder();
    static bool ResolveHarvest();
    void* GetQuotaFunction(int32_t SlotFromBound);
    static void PostUpdateHK(ABuildingSMActor* This, uint8_t State, FFortBuildingRandomList* RandomList);
    static void AttemptSpawnResourcesHK(ABuildingSMActor* This, AFortPawn* InstigatorPawn, float Damage, bool bJustHitWeakspot);
};
