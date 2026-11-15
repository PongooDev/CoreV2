#include "pch.h"
#include "Athena/FortAthenaExitCraftSpawner.h"
#include "Athena/FortAthenaExitCraft.h"
#include "Athena/FortAthenaExitCraftInfo.h"
#include "Athena/FortGameStateAthena.h"
#include "Building/BuildingActor.h"
#include "Building/BuildingFoundation.h"
#include "Building/BuildingSMActor.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/World.h"
#include "Finder.h"
#include "FortSpecialActorReplicationInfo.h"
#include "GameplayTagContainer.h"
#include "Hooks.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Log.h"
#include "Logging/LogMacros.h"
#include "Natives.h"
#include "Templates/Casts.h"
#include "TimerManager.h"
#include "UObject/Class.h"

#include <algorithm>
#include <cstring>
#include <vector>

DEFINE_LOG_CATEGORY_STATIC(LogAthenaExitCraftSpawner, Log, All);

class AController;

namespace
{
    // A building piece the clearing takes: one still standing, and neither a plot (ABuildingFoundation: its whole
    // streamed building or POI goes with it) nor a piece the map keeps invulnerable.
    bool IsBlockingPiece(ABuildingSMActor* Piece)
    {
        return Piece && !Piece->IsPendingKill() && !Piece->bDestroyed && !(ABuildingFoundation::StaticClass() && Piece->IsA(ABuildingFoundation::StaticClass()))
            && !(ABuildingActor::HasInvulnerable() && Piece->bIsInvulnerable);
    }

    // The piece dies as a prop or tree in the way of a player's new piece does (AFortPlayerController's building:
    // ABuildingActor::Die with Gameplay.Effect.InstantDeath.StructuralSupport, a gameplay tag passed by value), so every
    // client sees it break. Where the build's Die isn't found it is just destroyed.
    void KillPiece(ABuildingSMActor* Piece)
    {
        if (Natives::ABuildingActor_DieIndex < 0)
        {
            Finder::ResolveBuildingRestorationLive(ABuildingSMActor::EditingPlayerOffset());
        }
        if (Natives::ABuildingActor_DieIndex < 0)
        {
            Piece->K2_DestroyActor();
            return;
        }
        const FGameplayTag StructuralDeath{ FName(L"Gameplay.Effect.InstantDeath.StructuralSupport") };
        uint64_t TagBits = 0;
        static_assert(sizeof(StructuralDeath) == sizeof(TagBits));
        std::memcpy(&TagBits, &StructuralDeath, sizeof(TagBits));
        CallVirtual<void>(Piece, Natives::ABuildingActor_DieIndex, TagBits, static_cast<AController*>(nullptr), static_cast<AActor*>(nullptr));
    }
}

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

void AFortAthenaExitCraftSpawner::DestroyBlockingActors()
{
#if WITH_SERVER_CODE
    // The van's landing spot was never cleared (#458): the spawner's BeginPlay (5.40 0xBAACB0) sets
    // DestroyBuildingActorHandle to call this after the craft info's SpawnDestructionInitialDelay, before the van is due,
    // but the client builds keep only its last line (0xBB2710: BuildingActorsToDestroy.Reset()). Nothing else in 5.40 or
    // 5.41 reads DestructionOverlapCapsule (made in the spawner's constructor, 0xBA3210), fills BuildingActorsToDestroy or
    // reads SpawnDestructionDelayBetweenPieces, so those were this function's. Without it the van, a physics body that
    // falls from ExitCraftZOffset (its Spawned state, 0xBD5830, lands it only once it has slowed to MinLandingSpeed, and
    // only then do the next states lift it and turn it upright), came down on whatever stood at the safe zone, trees and
    // houses (#458's van lay on its roof among trees). The pieces inside the capsule die one every
    // SpawnDestructionDelayBetweenPieces, on DestroyBuildingActorHandle, as the members say; where the build gives no
    // delay (or the handle wasn't found) they go at once.
    UFortAthenaExitCraftInfo* Info = ExitCraftInfo;
    UWorld* World = GetWorld();
    if (Role != ROLE_Authority || !World || !Member_BuildingActorsToDestroy.IsAvailable())
    {
        return;
    }

    TArray<ABuildingSMActor*>& Pieces = GetMember_BuildingActorsToDestroy();
    if (Pieces.Num() == 0)
    {
        // The pieces in the capsule: the world's overlap query with its shape (its bounds: the spawner turns only about
        // Z, so the capsule stands upright), over every object type, for the building actors among what it finds.
        UPrimitiveComponent* Capsule = DestructionOverlapCapsule;
        FVector Origin;
        FVector Extent;
        float SphereRadius = 0.0f;
        UKismetSystemLibrary::GetComponentBounds(Capsule, Origin, Extent, SphereRadius);
        const float Radius = std::min(Extent.X, Extent.Y);
        if (!Capsule || Radius <= 0.0f || Extent.Z <= 0.0f)
        {
            UE_LOG(LogAthenaExitCraftSpawner, Warning, TEXT("DestroyBlockingActors: %s has no capsule to clear"), *GetName());
            return;
        }
        TArray<uint8_t> ObjectTypes;
        for (uint8_t ObjectType = 0; ObjectType < 32; ++ObjectType)
        {
            ObjectTypes.Add(ObjectType);
        }
        TArray<AActor*> Ignored;
        Ignored.Add(this);
        TArray<AActor*> Found = UKismetSystemLibrary::CapsuleOverlapActors(this, Origin, Radius, Extent.Z, ObjectTypes, ABuildingSMActor::StaticClass(), Ignored);
        for (int32_t Index = 0; Index < Found.Num(); ++Index)
        {
            ABuildingSMActor* Piece = Cast<ABuildingSMActor>(Found[Index]);
            if (IsBlockingPiece(Piece))
            {
                Pieces.Add(Piece);
            }
        }
        UE_LOG(LogAthenaExitCraftSpawner, Log, TEXT("DestroyBlockingActors: %s clears %d building pieces (capsule %.0f x %.0f at (%.0f, %.0f, %.0f))"), *GetName(), Pieces.Num(),
            Radius, Extent.Z, Origin.X, Origin.Y, Origin.Z);
    }

    float Delay = 0.0f;
    if (Info)
    {
        const FExitCraftInfo& CraftInfo = Info->ExitCraftInfo;
        const FScalableFloat& DelayBetweenPieces = CraftInfo.SpawnDestructionDelayBetweenPieces;
        Delay = DelayBetweenPieces.GetValueAtLevel(0.0f);
    }
    const bool bOneAtATime = Delay > 0.0f && Natives::AFortAthenaExitCraftSpawner_DestroyBuildingActorHandle >= 0;
    while (Pieces.Num() > 0)
    {
        ABuildingSMActor* Piece = Pieces[Pieces.Num() - 1];
        Pieces.RemoveAt(Pieces.Num() - 1);
        if (IsBlockingPiece(Piece))
        {
            KillPiece(Piece);
            if (bOneAtATime)
            {
                break;
            }
        }
    }

    if (Pieces.Num() > 0)
    {
        World->GetTimerManager().SetTimer(DestroyBuildingActorHandle, FTimerDelegate::CreateUObject(this, &AFortAthenaExitCraftSpawner::DestroyBlockingActors), Delay, false);
        return;
    }
#endif
    // What the build keeps: the list is done with.
    if (Member_BuildingActorsToDestroy.IsAvailable())
    {
        GetMember_BuildingActorsToDestroy().Reset();
    }
}

DEFINE_FUNCTION(AFortAthenaExitCraftSpawner::execDestroyBlockingActors)
{
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<AFortAthenaExitCraftSpawner*>(Context)->DestroyBlockingActors();
    P_NATIVE_END;
}

void AFortAthenaExitCraftSpawner::SpawnExitCraftHK(AFortAthenaExitCraftSpawner* This)
{
    This->SpawnExitCraft();
}

void AFortAthenaExitCraftSpawner::DestroyBlockingActorsHK(AFortAthenaExitCraftSpawner* This)
{
    This->DestroyBlockingActors();
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

    // The landing spot's clearing: BeginPlay binds what the build keeps of DestroyBlockingActors to
    // DestroyBuildingActorHandle; DestroyBlockingActors takes its place there and sets that handle again for each next
    // piece. Read before its native is replaced below (the finder follows the exec's jump).
    UFunction* DestroyFunction = FindStaticFunction_DestroyBlockingActors();
    const std::vector<Finder::FExitCraftSpawnerTimerBind> DestroyBinds = DestroyFunction && FTimerManager::ResolveNatives() && Member_ExitCraftInfo.IsAvailable()
        ? Finder::DestroyBlockingActorsBinds(DestroyFunction, SpawnerVTable, ActorVTable, Member_ExitCraftInfo.GetOffsetChecked())
        : std::vector<Finder::FExitCraftSpawnerTimerBind>();
    const ptrdiff_t ActorSize = static_cast<ptrdiff_t>(ActorClass->GetPropertiesSize());
    const ptrdiff_t SpawnerSize = static_cast<ptrdiff_t>(Class->GetPropertiesSize());
    for (const Finder::FExitCraftSpawnerTimerBind& Bind : DestroyBinds)
    {
        // The handle is one of the spawner's own members.
        if (Bind.Handle >= ActorSize && Bind.Handle + static_cast<ptrdiff_t>(sizeof(FTimerHandle)) <= SpawnerSize)
        {
            Natives::AFortAthenaExitCraftSpawner_DestroyBuildingActorHandle = Bind.Handle;
        }
        Hooks::RedirectAddress(L"AFortAthenaExitCraftSpawner::DestroyBlockingActors (DestroyBuildingActorHandle)", Bind.Lea, reinterpret_cast<void*>(&DestroyBlockingActorsHK));
    }
    if (DestroyBinds.empty())
    {
        Log::Error(L"Getaway: the van spawners' DestroyBlockingActors timer wasn't found, so their landing spots aren't cleared");
    }
    else
    {
        Log::Info(L"Getaway: DestroyBlockingActors bound at {} places, DestroyBuildingActorHandle +0x{:X}", DestroyBinds.size(), Natives::AFortAthenaExitCraftSpawner_DestroyBuildingActorHandle);
    }
    if (DestroyFunction && (Natives::FFrame_Code >= 0 || Finder::ResolveScriptFrame()))
    {
        DestroyFunction->SetNativeFunc(&execDestroyBlockingActors);
    }

    // The map icons the van and its spawner swap.
    if (!AFortSpecialActorReplicationInfo::ResolveNatives())
    {
        Log::Error(L"Getaway: the special actor list's natives weren't found, so the vans come without their map icon and the spawners' stay");
    }
#endif
}
