#include "pch.h"
#include "Athena/FortAthenaExitCraftSpawner.h"
#include "Athena/FortAthenaExitCraft.h"
#include "Athena/FortAthenaExitCraftInfo.h"
#include "Athena/FortGameStateAthena.h"
#include "Engine/World.h"
#include "Finder.h"
#include "FortSpecialActorReplicationInfo.h"
#include "Hooks.h"
#include "Kismet/GameplayStatics.h"
#include "Log.h"
#include "Logging/LogMacros.h"
#include "Natives.h"
#include "Templates/Casts.h"
#include "TimerManager.h"
#include "UObject/Class.h"

#include <vector>

DEFINE_LOG_CATEGORY_STATIC(LogAthenaExitCraftSpawner, Log, All);

void AFortAthenaExitCraftSpawner::SpawnExitCraft()
{
#if WITH_SERVER_CODE
    // 5.40 and 5.41 keep everything around this timer and strip what it calls: the spawner's BeginPlay (5.40 0xBAACB0)
    // and StartExitCraftSpawnTimer (0xBCAF80) bind the shared empty function to ExitCraftTimerHandle, so the "on the way"
    // icon stood at the next safe zone for the rest of the match and no van came. Nothing else in either build spawns
    // the craft info's ExitCaftClass, reads its ExitCraftZOffset or SpecialActorCraftTag, sets a craft's ExitCraftInfo
    // or CraftSpecialActorID, or broadcasts OnExitCraftSpawned, so those were this function's:
    // - The van comes out of the sky: Bling_Van_C's Spawned state shows its portal and falling trails where it appears,
    //   and its state machine (0xBCC8C0) waits for it to slow to the craft info's MinLandingSpeed before it lands. So it
    //   is spawned ExitCraftZOffset (VanInfo.SpawnHeight) above the spawner, which the map info put on the ground at the
    //   safe zone (0xBCA6F0), facing the spawner's random way. Deferred, as the map info spawns the spawner: its
    //   ExitCraftInfo (replicated; the states read it) is set before it begins play.
    // - Its icon goes on the map and the spawner's comes off (the special actor list's ids, which the departure takes
    //   the van's off by again, 0xBB2610).
    // - The mutator learns of it: its OnExitCraftSpawned (0xC5A150), bound when it spawned the spawner (0xC67CD0), moves
    //   the spawner's entry in SpawnedExitCraftList to the van and watches the van's OnExitCraftDestroying.
    UFortAthenaExitCraftInfo* Info = ExitCraftInfo;
    UClass* CraftClass = Info ? static_cast<UClass*>(Info->ExitCaftClass) : nullptr;
    if (Role != ROLE_Authority || !CraftClass)
    {
        return;
    }

    FTransform SpawnTransform = GetTransform();
    const FExitCraftInfo& CraftInfo = Info->ExitCraftInfo;
    const FScalableFloat& ZOffset = CraftInfo.ExitCraftZOffset;
    SpawnTransform.Translation.Z += ZOffset.GetValueAtLevel(0.0f);
    AFortAthenaExitCraft* Craft = Cast<AFortAthenaExitCraft>(UGameplayStatics::BeginDeferredActorSpawnFromClass(this, CraftClass, SpawnTransform, ESpawnActorCollisionHandlingMethod::AlwaysSpawn));
    if (!Craft)
    {
        UE_LOG(LogAthenaExitCraftSpawner, Error, TEXT("SpawnExitCraft: %s couldn't spawn its %s"), *GetName(), *CraftClass->GetName());
        return;
    }
    Craft->ExitCraftInfo = Info;
    UGameplayStatics::FinishSpawningActor(Craft, SpawnTransform);

    Craft->RegisterCraftSpecialActor();
    UnregisterSpawnerSpecialActor();
    if (Natives::AFortAthenaExitCraftSpawner_OnExitCraftSpawned >= 0 && DelegateInstance::IsLayoutResolved())
    {
        OnExitCraftSpawned.Broadcast(Craft, this);
    }
    const FVector Location = SpawnTransform.GetLocation();
    UE_LOG(LogAthenaExitCraftSpawner, Log, TEXT("SpawnExitCraft: %s brought in %s at (%.0f, %.0f, %.0f), on the map as %s"), *GetName(), *Craft->GetName(),
        Location.X, Location.Y, Location.Z, *static_cast<FName>(Craft->CraftSpecialActorID).ToString());
#endif
}

void AFortAthenaExitCraftSpawner::UnregisterSpawnerSpecialActor()
{
#if WITH_SERVER_CODE
    const FName SpecialActorID = SpawnerSpecialActorID;
    if (Role != ROLE_Authority || SpecialActorID.IsNone() || !AFortSpecialActorReplicationInfo::ResolveNatives())
    {
        return;
    }
    UWorld* World = GetWorld();
    AFortGameStateAthena* GameState = World ? Cast<AFortGameStateAthena>(World->GetGameState()) : nullptr;
    if (AFortSpecialActorReplicationInfo* SpecialActorData = GameState ? static_cast<AFortSpecialActorReplicationInfo*>(GameState->SpecialActorData) : nullptr)
    {
        SpecialActorData->RemoveActorFromReplicationList(SpecialActorID);
    }
#endif
}

void AFortAthenaExitCraftSpawner::SpawnExitCraftHK(AFortAthenaExitCraftSpawner* This)
{
    This->SpawnExitCraft();
}

void AFortAthenaExitCraftSpawner::HookNatives()
{
#if WITH_SERVER_CODE
    // From 5.40 (The Getaway); builds without the spawner have no vans.
    UClass* Class = StaticClass();
    UObject* SpawnerDefault = Class ? Class->GetDefaultObject(false) : nullptr;
    UClass* ActorClass = AActor::StaticClass();
    UObject* ActorDefault = ActorClass ? ActorClass->GetDefaultObject(false) : nullptr;
    if (!SpawnerDefault || !ActorDefault)
    {
        return;
    }

    // OnExitCraftSpawned is the spawner's first member (21.40's declaration; 5.40's mutator binds it at +0x328, AActor's
    // size), before DestructionOverlapCapsule, and the default object has it as Epic's constructor left it.
    const ptrdiff_t Delegate = (static_cast<ptrdiff_t>(ActorClass->GetPropertiesSize()) + 7) & ~static_cast<ptrdiff_t>(7);
    const ptrdiff_t Capsule = Member_DestructionOverlapCapsule.IsAvailable() ? Member_DestructionOverlapCapsule.GetOffsetChecked() : -1;
    if (Delegate > 0 && Delegate < Capsule && FMulticastDelegateBase::IsDefaultConstructedAt(reinterpret_cast<const uint8_t*>(SpawnerDefault) + Delegate))
    {
        Natives::AFortAthenaExitCraftSpawner_OnExitCraftSpawned = Delegate;
    }
    else
    {
        Log::Error(L"Getaway: AFortAthenaExitCraftSpawner::OnExitCraftSpawned not found (+0x{:X}, DestructionOverlapCapsule +0x{:X}), so the mutator won't know of the vans that come",
            Delegate, Capsule);
    }

    // The timer the van comes from: StartExitCraftSpawnTimer's body and the spawner's BeginPlay each bind the stripped
    // method with a lea; SpawnExitCraft takes its place there.
    void** SpawnerVTable = *reinterpret_cast<void***>(SpawnerDefault);
    void** ActorVTable = *reinterpret_cast<void***>(ActorDefault);
    const std::vector<uint8_t*> Binds = FTimerManager::ResolveNatives() && Member_ExitCraftInfo.IsAvailable()
        ? Finder::ExitCraftSpawnTimerBinds(FindStaticFunction_StartExitCraftSpawnTimer(), SpawnerVTable, ActorVTable, Member_ExitCraftInfo.GetOffsetChecked())
        : std::vector<uint8_t*>();
    if (Binds.empty())
    {
        Log::Error(L"Getaway: the van spawners' timer wasn't found, so no van comes");
    }
    else if (Binds.size() != 2)
    {
        Log::Warning(L"Getaway: the van spawners' timer is bound at {} places, not BeginPlay's and StartExitCraftSpawnTimer's 2", Binds.size());
    }
    for (uint8_t* Bind : Binds)
    {
        Hooks::RedirectAddress(L"AFortAthenaExitCraftSpawner::SpawnExitCraft (ExitCraftTimerHandle)", Bind, reinterpret_cast<void*>(&SpawnExitCraftHK));
    }

    // The map icons the van and its spawner swap.
    if (!AFortSpecialActorReplicationInfo::ResolveNatives())
    {
        Log::Error(L"Getaway: the special actor list's natives weren't found, so the vans come without their map icon and the spawners' stay");
    }
#endif
}
