#pragma once

#include "Building/BuildingSMActor.h"
#include "Math/Rotator.h"
#include "Math/Vector.h"

class AFortPawn;
class AFortPickup;
class AFortPlayerController;
class UFortWorldItemDefinition;
class UWorld;

struct FFortSearchBounceData
{
    DefineUnrealStruct(FFortSearchBounceData, FortniteGame)

public:
    DefineUProperty(FVector, BounceNormal)
    DefineUProperty(uint32_t, SearchAnimationCount)
};

class ABuildingContainer : public ABuildingSMActor
{
    DefineUnrealClass(ABuildingContainer, FortniteGame)
    DefineUFunction(BounceContainer)

public:
    DefineUProperty(FName, SearchLootTierGroup)
    DefineUStructProperty(FChosenQuotaInfo, SearchLootTierChosenQuotaInfo)
    DefineUProperty(FName, ContainerLootTierKey)
    DefineUProperty(UFortWorldItemDefinition*, LootTestingData)
    DefineUProperty(float, LootNoiseRange)
    DefineUProperty(FVector, LootSpawnLocation)
    DefineUProperty(uint8_t, HighestRarity)
    DefineBitfieldUProperty(bAlwaysShowContainer)
    DefineBitfieldUProperty(bAlwaysMaintainLoot)
    DefineBitfieldUProperty(bDestroyContainerOnSearch)
    DefineUProperty(float, LootedWeaponsDurabilityModifier)
    DefineUStructProperty(FFortSearchBounceData, SearchBounceData)
    // From 1.7.2 (Battle Royale): loot is tossed onto the ground, and floor loot is out from the start.
    DefineUProperty(FRotator, LootTossDirection_Athena)
    DefineUProperty(float, LootTossSpeed_Athena)
    DefineUProperty(float, LootTossConeHalfAngle_Athena)
    DefineBitfieldUProperty(bStartAlreadySearched_Athena)

    void PostLoad();
    void PostUpdate(EFortBuildingPersistentState State, FFortBuildingRandomList* RandomList);
    bool SpawnLoot(AFortPawn* Looter);
    void BounceContainer();

    static void HookVTables();
    static void LogWorldContainers(UWorld* World);

private:
    bool IsAlreadySearched(AFortPawn* Looter);
    void ModifyLootDrops(TStructArray<FFortItemEntry>& LootDrops, AFortPlayerController* Looter);
    void OnPickupSpawn(AFortPickup* Pickup, AFortPlayerController* Looter);
    void HideContainer();
    void SetSearched(AFortPawn* Searcher);
    void UpdateReplicatedLootTier();
    FName GetSearchLootTierGroup();
    bool StartsAlreadySearched();
    void SpawnFloorLoot();
    void TossLoot(AFortPickup* Pickup, const FVector& SpawnLocation);

    static void PostLoadHK(ABuildingContainer* This);
    static void PostUpdateHK(ABuildingContainer* This, uint8_t State, FFortBuildingRandomList* RandomList);
    static bool SpawnLootHK(ABuildingContainer* This, AFortPawn* Looter);
    static inline void (*PostLoadOG)(ABuildingContainer* This) = nullptr;
};
