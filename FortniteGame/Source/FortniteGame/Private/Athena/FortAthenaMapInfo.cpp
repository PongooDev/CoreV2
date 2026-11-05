#include "pch.h"
#include "Athena/FortAthenaMapInfo.h"
#include "Athena/FortAthenaSupplyDrop.h"
#include "Athena/FortGameStateAthena.h"
#include "Engine/World.h"
#include "Finder.h"
#include "GameFramework/Volume.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Log.h"
#include "Math/Rotator.h"
#include "Math/Transform.h"
#include "Math/Vector.h"
#include "Natives.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"

#include <algorithm>
#include <vector>

bool AFortAthenaMapInfo::HasLlamas()
{
    return Member_LlamaClass.IsAvailable() && Member_LlamaQuantityMin.IsAvailable() && Member_LlamaQuantityMax.IsAvailable()
        && Finder::ResolveSupplyDropSpawnLive(Member_LlamaClass.GetOffsetChecked());
}

FVector AFortAthenaMapInfo::PickSupplyDropLocation(const FVector& Center, float Radius)
{
    FVector Result = Center;
    if (Natives::AFortAthenaMapInfo_PickSupplyDropLocation)
    {
        Natives::AFortAthenaMapInfo_PickSupplyDropLocation(this, &Result, Center, Radius);
    }
    return Result;
}

AFortAthenaSupplyDrop* AFortAthenaMapInfo::SpawnSupplyDropAtLocation(const FVector& Location, const FRotator& Rotation, UClass* SupplyDropClass)
{
    return Natives::AFortAthenaMapInfo_SpawnSupplyDropAtLocation ? Natives::AFortAthenaMapInfo_SpawnSupplyDropAtLocation(this, Location, Rotation, SupplyDropClass) : nullptr;
}

namespace
{
    // The colliding bounds of the map's water (_WaterMeshBlueprintMaster_C: Loot Lake, the rivers and the sea). Its
    // collision only overlaps, so the drops' ground trace goes through it to the bed below.
    std::vector<FBox> FindWaterBounds(UWorld* World)
    {
        std::vector<FBox> Water;
        UClass* WaterClass = FindObject<UClass>(nullptr, L"/Game/Packages/DS_Fortnite_Terrain_NoLOD/Blueprints/_WaterMeshBlueprintMaster._WaterMeshBlueprintMaster_C");
        for (TObjectIterator<AActor> It; WaterClass && It; ++It)
        {
            AActor* Actor = *It;
            if (Actor->GetWorld() != World || Actor->HasAnyFlags(static_cast<EObjectFlags>(RF_ClassDefaultObject | RF_ArchetypeObject)) || !Actor->IsA(WaterClass))
            {
                continue;
            }
            FVector Origin;
            FVector Extent;
            Actor->GetActorBounds(true, Origin, Extent);
            if (Extent.X > 0.0f && Extent.Y > 0.0f)
            {
                Water.emplace_back(Origin - Extent, Origin + Extent);
            }
        }
        return Water;
    }

    // Whether the first thing a trace down from Location hits is the island's landscape, above the water: what the drop's
    // ground trace would put the llama on. Not a tree top, a roof, Loot Lake's surface (BP_Athena_Water_C blocks) or a
    // river's bed.
    bool IsOpenGround(UObject* WorldContextObject, const FVector& Location, UClass* LandscapeClass, const std::vector<FBox>& Water)
    {
        FVector Ground;
        UObject* GroundActor = nullptr;
        UObject* GroundComponent = nullptr;
        if (!UKismetSystemLibrary::LineTraceSingle(WorldContextObject, Location, FVector(Location.X, Location.Y, Location.Z - 200000.0f), 0, false, false, Ground, GroundActor, GroundComponent)
            || !GroundActor || !GroundActor->IsA(LandscapeClass))
        {
            return false;
        }
        return std::none_of(Water.begin(), Water.end(), [&Ground](const FBox& Body)
        {
            return Ground.X > Body.Min.X && Ground.X < Body.Max.X && Ground.Y > Body.Min.Y && Ground.Y < Body.Max.Y && Ground.Z < Body.Max.Z;
        });
    }
}

void AFortAthenaMapInfo::SpawnLlamas()
{
#if WITH_SERVER_CODE
    // The Supply Llamas are new in 3.3, and this is stripped with the rest of the server-only code: no client build reads
    // LlamaQuantityMin or LlamaQuantityMax, and only AFortGameModeAthena's "SPAWNLLAMA" console command (3.3 0x9E8230)
    // spawns one. The llama itself is native and Blueprint (AthenaSupplyDrop_Llama_C, an AFortAthenaSupplyDrop):
    // searching it rolls Loot_AthenaLlama with UFortKismetLibrary::PickLootDrops and drops each item with SpawnPickup.
    //
    // Between LlamaQuantityMin and LlamaQuantityMax of LlamaClass (AthenaGameData's Default.Llamas rows, 3 and 3 on 3.3),
    // each at a random point of the island, on the ground, facing a random way. The quantity is rolled as 3.3's safe zone
    // supply drops roll theirs (0xA039C0: Min plus a random 0 to Max - Min), and each llama is placed as "SPAWNLLAMA"
    // places one: a random yaw and SpawnSupplyDropAtLocation, at a point PickSupplyDropLocation picked inside
    // AircraftDropVolume (SpawnSupplyDropAtLocation's FindGroundLocationAt reads the placement heights from this map info).
    UClass* Llama = static_cast<UClass*>(LlamaClass);
    if (!Llama)
    {
        Log::Info(L"Battle Royale: the map info has no llama class, no Supply Llamas");
        return;
    }

    const int32_t QuantityMin = static_cast<int32_t>(LlamaQuantityMin.GetValueAtLevel(0.0f));
    const int32_t QuantityMax = static_cast<int32_t>(LlamaQuantityMax.GetValueAtLevel(0.0f));
    const int32_t Range = QuantityMax - QuantityMin;
    const int32_t Quantity = QuantityMin + (Range + 1 > 0 ? std::min(static_cast<int32_t>(FMath::FRand() * static_cast<float>(Range + 1)), Range) : 0);

    // The island: the drop volume's bounds (PickSupplyDropLocation keeps its points inside the volume), else the drop zone.
    FVector Center;
    float Radius = 0.0f;
    if (AVolume* DropVolume = AircraftDropVolume)
    {
        FVector Extent;
        DropVolume->GetActorBounds(false, Center, Extent);
        Radius = std::max(Extent.X, Extent.Y);
    }
    else if (const FBox2D DropZone = AircraftDropZone; DropZone.bIsValid)
    {
        Center = FVector((DropZone.MinX + DropZone.MaxX) * 0.5f, (DropZone.MinY + DropZone.MaxY) * 0.5f, 0.0f);
        Radius = std::max(DropZone.MaxX - DropZone.MinX, DropZone.MaxY - DropZone.MinY) * 0.5f;
    }
    if (Radius <= 0.0f)
    {
        Log::Error(L"Battle Royale: the map info has no drop volume or drop zone, no Supply Llamas");
        return;
    }

    // Core's addition: Epic's placement isn't known, and a point PickSupplyDropLocation picks can be over a tree, a
    // building or water, where the ground trace puts the llama on a tree top, a roof or Loot Lake. Such a point is picked
    // again (up to 50 times; about half of the island's points are open ground).
    UClass* LandscapeClass = FindObject<UClass>(nullptr, L"/Script/Landscape.LandscapeProxy");
    const std::vector<FBox> Water = FindWaterBounds(GetWorld());
    int32_t Spawned = 0;
    int32_t Repicked = 0;
    for (int32_t Index = 0; Index < Quantity; ++Index)
    {
        FVector Location = PickSupplyDropLocation(Center, Radius);
        for (int32_t Try = 0; LandscapeClass && Try < 50 && !IsOpenGround(this, Location, LandscapeClass, Water); ++Try, ++Repicked)
        {
            Location = PickSupplyDropLocation(Center, Radius);
        }
        const FRotator Rotation(0.0f, FMath::FRand() * 360.0f, 0.0f);
        AFortAthenaSupplyDrop* SupplyDrop = SpawnSupplyDropAtLocation(Location, Rotation, Llama);
        if (!SupplyDrop)
        {
            continue;
        }
        ++Spawned;
        const FVector Placed = SupplyDrop->GetTransform().GetLocation();
        Log::Info(L"Battle Royale: Supply Llama {} at ({:.0f}, {:.0f}, {:.0f})", GetNameSafe(SupplyDrop).ToStdString(), Placed.X, Placed.Y, Placed.Z);
    }
    Log::Info(L"Battle Royale: {} of {} Supply Llamas spawned (quantity {}-{}), {} points off open ground picked again", Spawned, Quantity, QuantityMin, QuantityMax, Repicked);
#endif
}
