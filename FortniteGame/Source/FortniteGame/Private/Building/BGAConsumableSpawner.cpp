#include "pch.h"
#include "Building/BGAConsumableSpawner.h"
#include "Athena/FortGameModeAthena.h"
#include "Engine/EngineTypes.h"
#include "Engine/World.h"
#include "EnvironmentQuery/EnvQueryInstanceBlueprintWrapper.h"
#include "EnvironmentQuery/EnvQueryManager.h"
#include "EnvironmentQuery/EnvQueryTypes.h"
#include "FortGameState.h"
#include "FortKismetLibrary.h"
#include "GameThread.h"
#include "Items/BGAConsumableWrapperItemDefinition.h"
#include "Items/FortLootPackage.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Log.h"
#include "Math/Quat.h"
#include "Math/Transform.h"
#include "Math/Vector.h"
#include "Templates/Casts.h"
#include "UObject/UObjectIterator.h"
#include "UObject/WeakObjectPtr.h"

#include <map>
#include <vector>

namespace
{
    // The placement query is asynchronous (UEnvQueryManager runs it over the next frames): its result is checked each
    // frame, for up to ten seconds at 30 Hz.
    constexpr int32_t QueryTicks = 300;

    // What the spawners of the current world spawned, taken out when the match's loot tables roll them again.
    struct FSpawnedConsumables
    {
        FWeakObjectPtr World;
        std::vector<FWeakObjectPtr> Actors;
    };
    FSpawnedConsumables Spawned;

    // The world's tally, logged once every spawner's query is done.
    struct FSpawnTally
    {
        int32_t Pending = 0;
        int32_t Spawners = 0;
        int32_t Actors = 0;
        int32_t Unplaced = 0;
        int32_t OnSpawner = 0;
        std::map<std::wstring, int32_t> ByClass;
    };
    FSpawnTally Tally;

    void LogTallyWhenDone()
    {
        if (--Tally.Pending > 0)
        {
            return;
        }
        std::wstring Classes;
        for (const auto& [Name, Count] : Tally.ByClass)
        {
            Classes += std::format(L"{}{} x{}", Classes.empty() ? L"" : L", ", Name, Count);
        }
        Log::Info(L"Battle Royale: {} BGA consumable spawners spawned {} building gameplay actors ({}), {} on a spawner without query points, {} without a place",
            Tally.Spawners, Tally.Actors, Classes.empty() ? std::wstring(L"none") : Classes, Tally.OnSpawner, Tally.Unplaced);
    }

    void WaitForQuery(FWeakObjectPtr Spawner, FWeakObjectPtr Query, int32_t TicksLeft)
    {
        GameThread::Enqueue([Spawner, Query, TicksLeft]()
        {
            UEnvQueryInstanceBlueprintWrapper* QueryInstance = static_cast<UEnvQueryInstanceBlueprintWrapper*>(Query.Get());
            if (QueryInstance && QueryInstance->GetResultsAsLocations().Num() == 0 && TicksLeft > 0)
            {
                WaitForQuery(Spawner, Query, TicksLeft - 1);
                return;
            }
            if (ABGAConsumableSpawner* Owner = static_cast<ABGAConsumableSpawner*>(Spawner.Get()))
            {
                Owner->OnQueryFinished(QueryInstance);
            }
            else
            {
                LogTallyWhenDone();
            }
        });
    }

    // The rotation that turns the actor's up onto the surface's normal (FQuat::FindBetweenNormals).
    FQuat AlignToSurface(const FVector& Normal)
    {
        const FVector Up(0.0f, 0.0f, 1.0f);
        const float W = 1.0f + (Up | Normal);
        FQuat Result;
        if (W < 1e-6f)
        {
            // Upside down: half a turn about X.
            Result.X = 1.0f;
            Result.W = 0.0f;
            return Result;
        }
        const FVector Axis = FVector::CrossProduct(Up, Normal);
        const float Size = FMath::Sqrt(Axis.X * Axis.X + Axis.Y * Axis.Y + Axis.Z * Axis.Z + W * W);
        Result.X = Axis.X / Size;
        Result.Y = Axis.Y / Size;
        Result.Z = Axis.Z / Size;
        Result.W = W / Size;
        return Result;
    }
}

void ABGAConsumableSpawner::SpawnWorldConsumables(UWorld* World)
{
#if WITH_SERVER_CODE
    if (!World || !StaticClass() || !UBGAConsumableWrapperItemDefinition::StaticClass())
    {
        return;
    }

    // A roll again: what the earlier one spawned goes first.
    int32_t Removed = 0;
    if (Spawned.World.Get() == World)
    {
        for (const FWeakObjectPtr& Actor : Spawned.Actors)
        {
            if (AActor* Consumable = static_cast<AActor*>(Actor.Get()))
            {
                Consumable->K2_DestroyActor();
                ++Removed;
            }
        }
    }
    Spawned.World = World;
    Spawned.Actors.clear();
    Tally = FSpawnTally();
    Tally.Pending = 1;

    AFortGameState* GameState = Cast<AFortGameState>(reinterpret_cast<UObject*>(World->GetGameState()));
    const int32_t WorldLevel = GameState && GameState->WorldManager ? static_cast<int32_t>(GameState->WorldLevel) : -1;
    for (TObjectIterator<ABGAConsumableSpawner> It; It; ++It)
    {
        ABGAConsumableSpawner* Spawner = *It;
        if (Spawner->GetWorld() != World || Spawner->HasAnyFlags(static_cast<EObjectFlags>(RF_ClassDefaultObject | RF_ArchetypeObject)) || Spawner->bDestroyed
            || Spawner->Role != ROLE_Authority)
        {
            continue;
        }
        ++Tally.Spawners;
        Spawner->SpawnConsumables(WorldLevel);
    }
    if (Removed > 0)
    {
        Log::Info(L"Battle Royale: the BGA consumables are rolled again, {} earlier ones taken out", Removed);
    }
    LogTallyWhenDone();
#endif
}

void ABGAConsumableSpawner::SpawnConsumables(int32_t WorldLevel)
{
#if WITH_SERVER_CODE
    // The roll: the tier group (through the game mode's Athena redirects, as the containers' groups go) gives wrapper items
    // whose Count is how many of its class to place.
    TStructArray<FFortItemEntry>& Consumables = ConsumablesToSpawn;
    Consumables.Empty();
    FName TierGroup = SpawnLootTierGroup;
    if (AFortGameModeAthena* GameMode = AFortGameModeAthena::GetCurrent(this))
    {
        TierGroup = GameMode->RedirectLootTierGroup(TierGroup);
    }
    FName LootTierKey;
    int32_t LootTier = -1;
    const FGameplayTagContainer& ContextTags = StaticGameplayTags;
    if (TierGroup.IsNone() || !UFortKismetLibrary::PickLootTierKeyAndLootTierFromTierGroup(LootTierKey, LootTier, TierGroup, WorldLevel, -1, ContextTags)
        || !UFortLootPackage::PickLootDrops(Consumables, WorldLevel, LootTierKey, nullptr, ContextTags, false, false) || Consumables.Num() == 0)
    {
        return;
    }

    // Where: the placement query's points around the spawner, best (random) score first.
    UEnvQuery* Query = QueryTemplate;
    UEnvQueryInstanceBlueprintWrapper* QueryInstance = Query ? UEnvQueryManager::RunEQSQuery(this, Query, this, EEnvQueryRunMode::AllMatching, nullptr) : nullptr;
    ++Tally.Pending;
    if (!QueryInstance)
    {
        OnQueryFinished(nullptr);
        return;
    }
    WaitForQuery(FWeakObjectPtr(this), FWeakObjectPtr(QueryInstance), QueryTicks);
#endif
}

void ABGAConsumableSpawner::OnQueryFinished(UEnvQueryInstanceBlueprintWrapper* QueryInstance)
{
#if WITH_SERVER_CODE
    // Each consumable on its own point; without the query's points, on the spawner, as many as the roll asks for.
    const TArray<FVector> Locations = QueryInstance ? QueryInstance->GetResultsAsLocations() : TArray<FVector>();
    UWorld* World = GetWorld();
    TStructArray<FFortItemEntry>& Consumables = ConsumablesToSpawn;
    int32_t NextLocation = 0;
    for (int32_t Index = 0; World && Index < Consumables.Num(); ++Index)
    {
        FFortItemEntry& Entry = Consumables[Index];
        UBGAConsumableWrapperItemDefinition* Wrapper = Cast<UBGAConsumableWrapperItemDefinition>(reinterpret_cast<UObject*>(static_cast<UFortItemDefinition*>(Entry.ItemDefinition)));
        UClass* ConsumableClass = Wrapper ? static_cast<TAssetSubclassOf<AActor>&>(Wrapper->ConsumableClass).LoadSynchronous() : nullptr;
        if (!ConsumableClass)
        {
            continue;
        }
        for (int32_t Count = 0; Count < static_cast<int32_t>(Entry.Count); ++Count)
        {
            if (NextLocation >= Locations.Num() && Locations.Num() > 0)
            {
                ++Tally.Unplaced;
                continue;
            }
            FTransform SpawnTransform;
            SpawnTransform.SetLocation(Locations.Num() > 0 ? Locations[NextLocation++] : GetTransform().GetLocation());
            Tally.OnSpawner += Locations.Num() == 0 ? 1 : 0;
            if (bAlignSpawnedActorsToSurface)
            {
                const FVector Location = SpawnTransform.GetLocation();
                FVector Ground;
                FVector Normal(0.0f, 0.0f, 1.0f);
                UObject* GroundActor = nullptr;
                UObject* GroundComponent = nullptr;
                if (UKismetSystemLibrary::LineTraceSingle(this, Location + FVector(0.0f, 0.0f, 50.0f), Location - FVector(0.0f, 0.0f, 100.0f), 0, false, true, Ground, GroundActor, GroundComponent, &Normal))
                {
                    SpawnTransform.Rotation = AlignToSurface(Normal);
                }
            }
            FActorSpawnParameters SpawnParameters;
            SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
            AActor* Consumable = World->SpawnActor(ConsumableClass, &SpawnTransform, SpawnParameters);
            if (!Consumable)
            {
                continue;
            }
            Spawned.Actors.emplace_back(Consumable);
            ++Tally.Actors;
            ++Tally.ByClass[ConsumableClass->GetName().ToStdString()];
        }
    }
    LogTallyWhenDone();
#endif
}
