#pragma once

#include "Building/BuildingActor.h"
#include "GameFramework/Info.h"
#include "GameplayTagContainer.h"
#include "Reflection.h"

#include <cstdint>

class AFortPlayerStartCreative;
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
    // own actors come back.
    void ResetVolumeAndLevelRecord(AFortVolume* Volume);

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
