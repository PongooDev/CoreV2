#pragma once

#include "Building/BuildingSMActor.h"
#include "Math/Rotator.h"
#include "Math/Vector.h"

#include <vector>

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
    DefineUProperty(FVector, LootFinalLocation)
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
    // What the container's IFortInteractInterface::ServerOnAttemptInteract does once a pawn's search is done (3.0
    // 0xA3E4F0): unless it is already searched, its SpawnLoot (the game's, or Core's where the build strips it). For a
    // searcher without a player controller (Battle Royale's player bots, #233). True when loot came out.
    bool ServerOnAttemptInteract(AFortPawn* InteractingPawn);
    bool IsAlreadySearched(AFortPawn* Looter);

    static void HookVTables();
    static void LogWorldContainers(UWorld* World);
    // Rolls the world's container tiers and floor loot again, from the loot tables as they are now.
    static void RollAthenaLootAgain(UWorld* World);

private:
    void ModifyLootDrops(TStructArray<FFortItemEntry>& LootDrops, AFortPlayerController* Looter);
    void OnPickupSpawn(AFortPickup* Pickup, AFortPlayerController* Looter);
    void HideContainer();
    void SetSearched(AFortPawn* Searcher);
    void UpdateReplicatedLootTier();
    FName GetSearchLootTierGroup();
    bool StartsAlreadySearched();
    void SpawnFloorLoot();
    FVector GetAthenaTossForward();
    float GetAthenaLootFront();
    FVector GetAthenaLootSpawnLocation();
    FVector GetAthenaLootTossVelocity();
    FVector GetAthenaLootFinalLocation();

    static void PostLoadHK(ABuildingContainer* This);
    static void PostUpdateHK(ABuildingContainer* This, uint8_t State, FFortBuildingRandomList* RandomList);
    static bool SpawnLootHK(ABuildingContainer* This, AFortPawn* Looter);
    static inline void (*PostLoadOG)(ABuildingContainer* This) = nullptr;
};
