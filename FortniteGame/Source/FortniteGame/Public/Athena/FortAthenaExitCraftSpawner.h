#pragma once

#include "Delegates/Delegate.h"
#include "GameFramework/Actor.h"
#include "ObjectLayout.h"

class AFortAthenaExitCraft;
class AFortAthenaExitCraftSpawner;
class UFortAthenaExitCraftInfo;
class UPrimitiveComponent;

// 21.40's DECLARE_MULTICAST_DELEGATE_TwoParams(FOnExitCraftSpawned, AFortAthenaExitCraft*, AFortAthenaExitCraftSpawner*).
using FOnExitCraftSpawned = TMulticastDelegate<AFortAthenaExitCraft*, AFortAthenaExitCraftSpawner*>;

// Where a Getaway van comes down (from 5.40; Bling_VanSpawner_C). The Getaway's mutator spawns one at the next safe
// zone when a van is due (5.40 0xC67CD0, through AFortAthenaMapInfo's spawner spawn 0xBCA6F0, which registers its
// special actor: the "on the way" icon) and binds OnExitCraftSpawned. The spawner's BeginPlay (0xBAACB0) sets its
// timers: DestroyBlockingActors after SpawnDestructionInitialDelay, and the van after ExitCraftSpawnDelay.
class AFortAthenaExitCraftSpawner : public AActor
{
    DefineUnrealClass(AFortAthenaExitCraftSpawner, FortniteGame)
    DefineUFunction(StartExitCraftSpawnTimer)

public:
    DefineUProperty(UPrimitiveComponent*, DestructionOverlapCapsule)
    DefineUProperty(UFortAthenaExitCraftInfo*, ExitCraftInfo)
    // The id the spawner's special actor has on the game state's list, which RegisterSpawnerSpecialActor sets.
    DefineUProperty(FName, SpawnerSpecialActorID)
    static ptrdiff_t SpawnerSpecialActorIDOffset() { return StaticClass() && Member_SpawnerSpecialActorID.IsAvailable() ? Member_SpawnerSpecialActorID.GetOffsetChecked() : -1; }
    // Its first member (unreflected): what the mutator's OnExitCraftSpawned (5.40 0xC5A150) is bound to.
    DefineNativeObject(FOnExitCraftSpawned, OnExitCraftSpawned, AFortAthenaExitCraftSpawner_OnExitCraftSpawned)

    // The van comes: what ExitCraftTimerHandle calls once ExitCraftSpawnDelay is up. The client builds strip its body
    // (5.40 and 5.41 bind the shared empty function in its place), so no van came. It spawns the craft info's van
    // ExitCraftZOffset above the spawner, facing its way, which falls to the spot and lands; puts the van's icon on the
    // map (AFortAthenaExitCraft::RegisterCraftSpecialActor) and takes the spawner's off; and tells the mutator, which
    // tracks the van from then on (OnExitCraftSpawned).
    void SpawnExitCraft();
    // Takes the spawner's icon off every map and compass, as the supply drop's unregister does its own (5.40 0xBCC820).
    void UnregisterSpawnerSpecialActor();

    // From 5.40: SpawnExitCraft where the build's BeginPlay and StartExitCraftSpawnTimer bind the stripped body to
    // ExitCraftTimerHandle (Hooks::RedirectAddress). Nothing on builds without exit crafts.
    static void HookNatives();

private:
    // The method the timer's delegate calls (this in rcx, as a member function's).
    static void SpawnExitCraftHK(AFortAthenaExitCraftSpawner* This);
};
