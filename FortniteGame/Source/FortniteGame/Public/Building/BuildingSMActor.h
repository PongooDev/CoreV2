#pragma once

#include "Building/BuildingActor.h"
#include "Items/FortLootPackage.h"
#include "StructArray.h"

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

// What callers pass to PostUpdate (inferred from its use): a list of rolls read round-robin, 0 when empty.
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
    DefineUProperty(FName, DestructionLootTierGroup)
    DefineUStructProperty(FChosenQuotaInfo, DestructionLootTierChosenQuotaInfo)
    DefineUProperty(FName, DestructionLootTierKey)

    void PostUpdate(EFortBuildingPersistentState State, FFortBuildingRandomList* RandomList);
    void SelectMeshSet(int32_t LootTier);
    void SelectMeshSet(FName LootTierKey);
    void SetMeshSet(const FMeshSet& MeshSet);

    static void HookVTables();

protected:
    AFortWorldManager* GetWorldManager() const;
    int32_t GetWorldLevel() const;
    void UpdateActorRecord(AFortWorldManager* WorldManager, EFortBuildingPersistentState State, bool bForce);

private:
    static void PostUpdateHK(ABuildingSMActor* This, uint8_t State, FFortBuildingRandomList* RandomList);
};
