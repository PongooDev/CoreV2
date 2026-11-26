#pragma once

#include "Building/BuildingActor.h"
#include "Components/ActorComponent.h"
#include "GameFramework/Info.h"
#include "GameplayTagContainer.h"
#include "Reflection.h"
#include "UObject/ScriptDelegates.h"

#include <cstdint>
#include <vector>

class AController;
class AFortAthenaCreativePortal;
class AFortPlayerController;
class AFortPlayerStartCreative;
class UAsyncTaskQueue;
class UFortPlaysetItemDefinition;
class UWorld;

// EVolumeState (7.00): Uninitialized, ReadOnly (a minigame is in progress: the island reverts when it ends),
// Initializing, Ready.
namespace EVolumeState
{
    inline constexpr uint8_t Uninitialized = 0;
    inline constexpr uint8_t ReadOnly = 1;
    inline constexpr uint8_t Initializing = 2;
    inline constexpr uint8_t Ready = 3;
}

struct FFortCreativeClassInstanceTracker
{
    DefineUnrealStruct(FFortCreativeClassInstanceTracker, FortniteGame)

public:
    DefineUProperty(TArray<UClass*>, ChildClassesAtLimit)
    DefineUProperty(TArray<UClass*>, ParentClassesAtLimit)

    // Epic's (8.51 0x125ADF0; 9.00 has no copy): whether NewActors, net of ActorsToRemove, add an actor of a class at
    // its limit.
    bool CanAddActors(const TArray<AActor*>& NewActors, const TArray<AActor*>& ActorsToRemove) const;

    static ptrdiff_t ChildClassesAtLimitOffset() { return Member_ChildClassesAtLimit.IsAvailable() ? Member_ChildClassesAtLimit.GetOffsetChecked() : -1; }
    static ptrdiff_t ParentClassesAtLimitOffset() { return Member_ParentClassesAtLimit.IsAvailable() ? Member_ParentClassesAtLimit.GetOffsetChecked() : -1; }
};

struct FMemoryBudget
{
    int32_t TotalMemory;
    int32_t InUseMemory;
    uint8_t Padding[8];
};

// An island's cost budget (Epic's FFortCreativeBudgetTracker).
struct FFortCreativeBudgetTracker
{
    DefineUnrealStruct(FFortCreativeBudgetTracker, FortniteGame)

public:
    DefineUProperty(FMemoryBudget, GlobalBudget)
    DefineUStructProperty(FFortCreativeClassInstanceTracker, ClassTracker)

    // Epic's (8.51 0x125A360, the one-actor form of 0x125A3F0; 9.00 has no copy): whether the island can take NewActor
    // in place of ActorsToRemove. Without removals it is the build's own CanAddActors (9.00 0x12F6B50).
    bool CanAddActor(AActor* NewActor, const TArray<AActor*>& ActorsToRemove) const;
    bool CanAddActors(const TArray<AActor*>& NewActors, const TArray<AActor*>& ActorsToRemove) const;

    // 8.51 0x126ECA0 (the asset cost, intact in 9.00 as 0x130D3B0): the asset's memory cost from the build's cost table.
    static int32_t CostUsedByAsset(const TArray<FName>& ActorId);
    // The instance cost the budget check adds per actor (from the same table, else Creative.FixedInstanceCost).
    static int32_t InstanceCostOf(const TArray<FName>& ActorId);
    // 8.51 0x1149FC0 (FCreativeActorId from an actor): its class's name, and a static mesh actor's mesh name.
    static TArray<FName> MakeActorId(const AActor* Actor);

    static ptrdiff_t ClassTrackerOffset() { return Member_ClassTracker.IsAvailable() ? Member_ClassTracker.GetOffsetChecked() : -1; }

private:
    // The tracker's unreflected TMap<FCreativeActorId, int32> of asset instance counts, after GlobalBudget (+0x20).
    const int32_t* FindAssetInstances(const TArray<FName>& ActorId) const;
};

// A Creative island's space (from 7.00; FortVolumeActor_Base_C in Athena_Playground): its playset (the island's own
// level, streamed by its UPlaysetLevelStreamComponent), its UFortLevelSaveComponent (what was built on it) and the
// rifts linked to it.
class AFortVolume : public ABuildingActor
{
    DefineUnrealClass(AFortVolume, FortniteGame)

public:
    DefineUProperty(UFortPlaysetItemDefinition*, CurrentPlayset)
    DefineUProperty(uint8_t, VolumeState)
    DefineBitfieldUProperty(bNeverAllowSaving)
    static bool HasNeverAllowSaving() { return Member_bNeverAllowSaving.IsAvailable(); }
    DefineUProperty(UAsyncTaskQueue*, TaskQueue)
    DefineUProperty(FMulticastScriptDelegate, OnFinishVolumeClear)
    // The hub's rifts linked to the island (each rift's LinkedVolume registers it here).
    DefineUProperty(TArray<AFortAthenaCreativePortal*>, LinkedPortals)
    static bool HasLinkedPortals() { return Member_LinkedPortals.IsAvailable(); }
    DefineUStructProperty(FFortCreativeBudgetTracker, BudgetTracker)

    // The volume's task queue, which its island's loads and resets run on (the game's own getter, 7.00 0xD8F7A0: TaskQueue,
    // made on first use). Null where the build's getter wasn't found.
    UAsyncTaskQueue* GetTaskQueue();
    // The queue step that empties the volume of what an island put there (the game's own, 7.00 0xD83860, the first step
    // of AFortVolumeManager::ResetVolumeAndLevelRecord after the read-only state): on the authority it clears the volume's
    // actors and returns OnFinishVolumeClear, which it broadcasts once they are gone; null elsewhere.
    FMulticastScriptDelegate* ClearVolumeOnServer();

    // TaskQueue's and OnFinishVolumeClear's offsets (-1 without them), for the finders of the two natives.
    static int32_t TaskQueueOffset();
    static int32_t OnFinishVolumeClearOffset();

    // Whether Location is inside the volume's box (its actor bounds, as the volume manager's GetVolumeForLocation).
    bool ContainsLocation(const FVector& Location) const;
    // The Creative player starts in the volume with Tag in their PlayerStartTags (any of them with none), nearest first
    // to Near.
    AFortPlayerStartCreative* FindPlayerStart(const wchar_t* Tag, const FVector& Near) const;
    // The island's start a player arriving on it takes (7.10 and 7.20, #485): its first player start set as an island
    // start (bUseAsIslandStart: a Player Spawner's "Use as Island Start", on by default) and linked to this volume, the
    // pick the game's own respawn place makes for a player on the island outside a game (7.20 0xE8BFF0, inlined in 7.10's
    // 0xDBAD80). Null where there is none, and on 7.00, whose player starts have no such setting.
    AFortPlayerStartCreative* FindIslandStart() const;
};

// What links a Creative actor to the island space it is in (from 7.00): the volume, once it is found (LinkedVolume).
class UFortCreativeVolumeLinkComponent : public UActorComponent
{
    DefineUnrealClass(UFortCreativeVolumeLinkComponent, FortniteGame)

public:
    DefineUProperty(AFortVolume*, LinkedVolume)
    static bool HasLinkedVolume() { return Member_LinkedVolume.IsAvailable(); }
};

// The match's island spaces (one per world, from 7.00): which volume an actor is in, and the resets and saves of each.
class AFortVolumeManager : public AInfo
{
    DefineUnrealClass(AFortVolumeManager, FortniteGame)
    DefineUFunction(GetVolumeForActor)
    DefineUFunction(GetAllPlayersInVolume)
    DefineUFunction(ResetVolumeAndLevelRecord)
    DefineUFunction(GetVolumeForLocation)
    DefineUFunction(ResaveActor)
    DefineUFunction(ResaveActorWithVolume)

public:
    // Broadcast by HandleVolumeStateChanged (8.00 0xEF5E70) with each registered volume's new EVolumeState (the volume's
    // SetVolumeState, 8.00 0xEEE730), on the server too.
    DefineUProperty(FMulticastScriptDelegate, OnVolumeStateChanged)
    static bool HasOnVolumeStateChanged() { return Member_OnVolumeStateChanged.IsAvailable(); }

    AFortVolume* GetVolumeForActor(const AActor* Actor);
    // The controllers of the players in Volume (the manager's VolumeActivePlayers), the game's own UFunction (from 7.00).
    // Empty where the build has none.
    TArray<AController*> GetAllPlayersInVolume(AFortVolume* Volume);
    // The game's own reset of an island (7.00 0xDA0E70): what was built goes, the record with it, and the playset's
    // own actors come back. 7.10 adds the player who asked for it (RequestingFortPC).
    void ResetVolumeAndLevelRecord(AFortVolume* Volume, AFortPlayerController* RequestingFortPC = nullptr);

    static AFortVolumeManager* Get(const UObject* WorldContextObject);
    // The game's own UFunctions (STAT_Find_Volume's lookup; the level save's update of an actor that changed).
    AFortVolume* GetVolumeForLocation(const FVector& Location);
    void ResaveActor(AActor* Actor);
    // The island's level record takes the actor's current state (9.00 0x1249F40). Nothing where the build lacks it.
    void ResaveActorWithVolume(AActor* Actor, AFortVolume* Volume);
};

// A player start of Creative's (from 7.00): the hub's, by each rift (PortalIndex), and the islands' own, by their tags:
// Playground.LobbyIsland.Spawn, Playground.IslandDock.Spawn, Creative.Island.Spawn.
class AFortPlayerStartCreative : public AActor
{
    DefineUnrealClass(AFortPlayerStartCreative, FortniteGame)

public:
    DefineUProperty(FGameplayTagContainer, PlayerStartTags)
    DefineUProperty(int32_t, PortalIndex)
    // 7.10 and later: whether the start is one a player arriving on the island takes (MinigamePlayerStartPlate, the
    // Player Spawner, sets its child start's from its UseAsIslandStart), and its link to the island space it is in.
    DefineUProperty(bool, bUseAsIslandStart)
    DefineUProperty(UFortCreativeVolumeLinkComponent*, CreativeLinkComponent)
    static bool HasUseAsIslandStart() { return Member_bUseAsIslandStart.IsAvailable(); }
    static bool HasCreativeLinkComponent() { return Member_CreativeLinkComponent.IsAvailable(); }

    bool HasTag(const wchar_t* Tag) const;
};
