#pragma once

#include "Containers/Array.h"
#include "GameFramework/Actor.h"
#include "Reflection.h"

class AFortPoiVolume;
class UWorld;

// Battle Royale's POI manager (3.0). AFortGameModeAthena::InitGame spawns it (the game mode keeps it unreflected at
// +0xBE8); HandleMatchHasStarted calls its InitalizePoiManger (3.0 0x9EE480), which puts every AFortPoiVolume in
// AllPoiVolumes and the ones ShouldTickUpdate says are small in SmallTickUpdatedPoi. Its tick (0xA05A70) tests up to
// MaxNumTestsPerFrame players a frame, round robin (TestPlayerIndex), against SmallTickUpdatedPoi
// (AFortPoiVolume::CheckPawnOverlap: tags the pawn, sends Visit). The stat senders (Interact, Land, eliminations) ask
// the game mode for a pawn's location tags (its virtual at slot 0x141, 0x9AD110), which asks GetTagsForPawnLocation
// (0x9EB7C0) to test AllPoiVolumes.
class AFortPoiManager : public AActor
{
    DefineUnrealClass(AFortPoiManager, FortniteGame)

public:
    DefineUProperty(TArray<AFortPoiVolume*>, SmallTickUpdatedPoi)
    DefineUProperty(TArray<AFortPoiVolume*>, AllPoiVolumes)
    DefineUProperty(int32_t, MaxNumTestsPerFrame)

    static AFortPoiManager* Get(UWorld* World);

    // Once the match has started and InitalizePoiManger has listed the volumes, puts the volumes Core shaped in
    // SmallTickUpdatedPoi by ShouldTickUpdate's own test on their shape, as long as World's manager is there.
    static void WatchInitialization(UWorld* World);
};
