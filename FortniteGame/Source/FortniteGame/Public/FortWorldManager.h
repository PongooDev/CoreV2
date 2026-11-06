#pragma once

#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "GameFramework/Info.h"
#include "GameFramework/OnlineReplStructs.h"
#include "GameplayTagContainer.h"
#include "Items/FortLootPackage.h"
#include "Math/Transform.h"
#include "Missions/FortMissionManager.h"
#include "UObject/AssetPtr.h"

class AFortPlayerController;
class FScriptArray;
class UFortItemCacheItemDefinition;
class UFortMissionGenerator;
class UFortMissionInfo;
class UFortWorldRecord;
class UWorld;
class UWorldTileManager;
struct FFortBuildingRandomList;
struct FFortPlayerSpawnPadPlacementData;
struct FQuickBar;

struct FFortStartingMissionInfo
{
    DefineUnrealStruct(FFortStartingMissionInfo, FortniteGame)
    DefineUProperty(TArray<UFortMissionInfo*>, StartingMissions)
    DefineBitfieldUProperty(bDisableSharedMissionLoading)
};

struct FFortLevelStreamingInfo
{
    DefineUnrealStruct(FFortLevelStreamingInfo, FortniteGame)

public:
    DefineUProperty(FName, PackageName)
};

struct FFortTileLootData
{
    DefineUnrealStruct(FFortTileLootData, FortniteGame)

public:
    DefineUStructProperty(FFortLootQuotaData, LootQuotas)
    DefineUProperty(int32_t, LootDrops)

    FFortLootQuotaData& GetLootQuota(uint8_t QuotaLevel) const;
    int32_t& GetLootDrops(uint8_t QuotaLevel) const;
};

struct FFortPlacementActorFinderEntry
{
    DefineUnrealStruct(FFortPlacementActorFinderEntry, FortniteGame)

public:
    DefineUProperty(TAssetSubclassOf<AActor>, BuildingToSpawn)
    DefineUProperty(TArray<UClass*>, BuildingClassesToFind)
    DefineUProperty(FGameplayTagContainer, RequiredTags)
    DefineUProperty(FGameplayTagContainer, PreferredTags)
    DefineUProperty(FGameplayTagContainer, ExlusionTags)
};

struct FFortPlacementActorFinderInfo
{
    DefineUnrealStruct(FFortPlacementActorFinderInfo, FortniteGame)

public:
    DefineUProperty(float, DistanceRangeMin)
    DefineUProperty(float, DistanceRangeMax)
    DefineUStructArrayProperty(FFortPlacementActorFinderEntry, BuildingData)
};

// The item caches a player picked up in the zone and the MCP hasn't given yet (4.0): the world manager's
// UnsavedPlayerEarnedItemCaches, one entry a player, by their net id.
struct FFortPlayerEarnedItemCaches
{
    DefineUnrealStruct(FFortPlayerEarnedItemCaches, FortniteGame)

public:
    DefineUStructProperty(FUniqueNetIdRepl, PlayerID)
    DefineUProperty(TArray<UFortItemCacheItemDefinition*>, EarnedItemCaches)
};

class EFortWorldManagerState
{
    DefineUnrealEnum(EFortWorldManagerState, FortniteGame)
    DefineUEnumValue(WMS_Created)
    DefineUEnumValue(WMS_QueryingWorld)
    DefineUEnumValue(WMS_WorldQueryComplete)
    DefineUEnumValue(WMS_CreatingNewWorld)
    DefineUEnumValue(WMS_LoadingExistingWorld)
    DefineUEnumValue(WMS_Running)
    DefineUEnumValue(WMS_Failed)
    DefineUEnumValue(WMS_MAX)
};

class AFortWorldManager : public AInfo
{
    DefineUnrealClass(AFortWorldManager, FortniteGame)

public:
    DefineUStructArrayProperty(FFortLevelStreamingInfo, StreamedLevels)
    DefineBitfieldUProperty(bConstructTileWorld)
    DefineUProperty(EFortWorldManagerState, WorldManagerState)
    DefineUProperty(UWorldTileManager*, TileManager)
    DefineUProperty(FName, DefaultLootQuotaCategory)
    DefineUStructProperty(FFortTileLootData, LootData)
    DefineUProperty(UFortWorldRecord*, CurrentWorldRecord)
    DefineUStructProperty(FFortStartingMissionInfo, StartingMissionInfo)
    DefineUStructProperty(FFortMissionManagerRecord, MissionManagerRecord)
    DefineBitfieldUProperty(bHasCreatedMissions)
    DefineBitfieldUProperty(bCreatedMissionRecord)
    DefineUProperty(bool, bZoneFinished)
    DefineUProperty(bool, bZoneCompleted)
    DefineUProperty(FVector, FriendlyPlayerSpawnExtent)
    DefineUProperty(FGameplayTagContainer, OverrideInitialPlayerSpawnTags)
    DefineUProperty(FGameplayTagContainer, OverrideBackupPlayerSpawnTags)
    DefineUProperty(float, ExplorationActorDistanceRangeMin)
    DefineUProperty(float, ExplorationActorDistanceRangeMax)
    DefineUStructArrayProperty(FFortPlayerEarnedItemCaches, UnsavedPlayerEarnedItemCaches)

    static ptrdiff_t ZoneFinishedOffset();
    static ptrdiff_t ZoneCompletedOffset();

    void OnWorldRecordQueryComplete();
    void ServerTickLoadingOrCreatingWorld();
    void AddSharedMissionsToMissionManagerRecord();
    void LoadQuestDrivenMissionsForPlayer(AFortPlayerController* PlayerController);
    void AddMissionsToMissionManagerRecord(const TArray<UFortMissionInfo*>& Missions, EMissionGenerationCategory GenerationCategory = EMissionGenerationCategory::GetMax_None(), UFortMissionGenerator* MissionGenerator = nullptr);
    void CreateMissionManager();
    int32_t GetOrAllocatePlayerID(const FString& UniqueId, const FString& DisplayName);
    int32_t GetWorldLevel() const;
    void SpawnTileLoot(FFortTileLootData* TileLootData, const TArray<int32_t>& LevelIndices);
    AActor* SpawnPlayerSpawnPad(const FFortPlayerSpawnPadPlacementData& PlacementData, const FTransform& SpawnTransform);
    bool UpdatePlayerQuickBarRecords(AFortPlayerController* PlayerController);
    bool RestorePlayerQuickBars(AFortPlayerController* PlayerController);

    // Whether the build keeps the item caches players pick up (4.0's UnsavedPlayerEarnedItemCaches).
    static bool HasEarnedItemCaches();
    // Keeps an item cache the player picked up for the end of the mission; false on a build without item caches.
    bool AddEarnedItemCache(AFortPlayerController* PlayerController, UFortItemCacheItemDefinition* ItemCache);
    // The item caches the player earned, as EndPrimaryMission's EarnedItemCaches has them (their template ids), and no
    // longer unsaved.
    void TakeEarnedItemCaches(AFortPlayerController* PlayerController, TArray<FString>& OutEarnedItemCaches);

    static void Hook();
    static void HookCallSites();

private:
    static inline void (*OnWorldRecordQueryCompleteOG)(AFortWorldManager* This) = nullptr;
    static void OnWorldRecordQueryCompleteHK(AFortWorldManager* This);
    static inline void (*ServerTickLoadingOrCreatingWorldOG)(AFortWorldManager* This) = nullptr;
    static void ServerTickLoadingOrCreatingWorldHK(AFortWorldManager* This);
    static inline void (*SendNewPlayerEventOG)(AFortPlayerController* PlayerController) = nullptr;
    static void SendNewPlayerEventHK(AFortPlayerController* PlayerController);
    static bool UpdatePlayerQuickBarRecordsCall(AFortWorldManager* This, AFortPlayerController* PlayerController);
    static void ConditionalFoundationQuotaCall(AFortWorldManager* This);
    static void GetLootQuotasForCategoryCall(const FName* QuotaCategory, FFortTileLootData* OutLootData, int32_t WorldLevel);
    static FFortTileLootData* GetTileRecordCall(AFortWorldManager* This, void* TileInfo, int32_t TileIndex, TArray<int32_t>& OutLevelIndices);
    static void HookWorldLoot();
    static AFortWorldManager* ConditionalFoundationContextCall(AFortWorldManager* This);
    static bool SelectConditionalFoundationsCall(AFortWorldManager* This, const void* Query, TArray<AActor*>& OutSelected, void* OutUnselected, void* RandomStream, float MinDistance, bool bFlag);
    static TArray<AActor*>* SpawnExplorationActorsCall(TArray<AActor*>* Result, UWorld* World, const FFortPlacementActorFinderInfo* FinderInfo, bool* bOutSucceeded, void* RandomStream);

    bool SelectConditionalFoundations(const FScriptArray& Requests, TArray<AActor*>& OutSelected, void* RandomStream);
    int32_t GetConditionalFoundationTierIndex() const;
    FFortBuildingRandomList* GetLootRandomList();

    bool GetPlayerQuickBarRecords(AFortPlayerController* PlayerController, FQuickBar*& OutPrimary, FQuickBar*& OutSecondary);
};
