#pragma once

#include "Building/BuildingActor.h"
#include "GameFramework/Info.h"
#include "GameplayTagContainer.h"
#include "Reflection.h"
#include "UObject/ScriptDelegates.h"

#include <cstdint>

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

// A Creative island's space (from 7.00; FortVolumeActor_Base_C in Athena_Playground): its playset (the island's own
// level, streamed by its UPlaysetLevelStreamComponent), its UFortLevelSaveComponent (what was built on it) and the
// rifts linked to it.
class AFortVolume : public ABuildingActor
{
    DefineUnrealClass(AFortVolume, FortniteGame)

public:
    DefineUProperty(UFortPlaysetItemDefinition*, CurrentPlayset)
    DefineUProperty(uint8_t, VolumeState)
    DefineUProperty(UAsyncTaskQueue*, TaskQueue)
    DefineUProperty(FMulticastScriptDelegate, OnFinishVolumeClear)

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
};

// The match's island spaces (one per world, from 7.00): which volume an actor is in, and the resets and saves of each.
class AFortVolumeManager : public AInfo
{
    DefineUnrealClass(AFortVolumeManager, FortniteGame)
    DefineUFunction(GetVolumeForActor)
    DefineUFunction(ResetVolumeAndLevelRecord)

public:
    AFortVolume* GetVolumeForActor(const AActor* Actor);
    // The game's own reset of an island (7.00 0xDA0E70): what was built goes, the record with it, and the playset's
    // own actors come back. 7.10 adds the player who asked for it (RequestingFortPC).
    void ResetVolumeAndLevelRecord(AFortVolume* Volume, AFortPlayerController* RequestingFortPC = nullptr);

    static AFortVolumeManager* Get(const UObject* WorldContextObject);
};

// A player start of Creative's (from 7.00): the hub's, by each rift (PortalIndex), and the islands' own, by their tags:
// Playground.LobbyIsland.Spawn, Playground.IslandDock.Spawn, Creative.Island.Spawn.
class AFortPlayerStartCreative : public AActor
{
    DefineUnrealClass(AFortPlayerStartCreative, FortniteGame)

public:
    DefineUProperty(FGameplayTagContainer, PlayerStartTags)
    DefineUProperty(int32_t, PortalIndex)

    bool HasTag(const wchar_t* Tag) const;
};
