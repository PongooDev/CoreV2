#pragma once

#include "Containers/Array.h"
#include "GameFramework/Actor.h"
#include "Math/Rotator.h"
#include "Math/Vector.h"
#include "Reflection.h"
#include "StructArray.h"
#include "UObject/NameTypes.h"

#include <cstring>

class UClass;
class UWorld;

// What a floating upgrade looks like for one loot table (GoosePickup_C's DisplayData: the chest's colour per rarity,
// the medkit for GooseHealth). OnRep_DisplayIndex sets DisplayData[DisplayIndex]'s mesh.
struct FGooseDisplayData
{
    DefineUnrealStruct(FGooseDisplayData, FortniteGame)
    DefineUProperty(FName, LootTableName)
};

// The pickup the mutator's timer asks for (8.40 CL-6005771, built by 0xFE2B60 and handed with the world to the
// stripped spawn): the mutator's PickupClass at a random spot over the play area with a random yaw, the loot table
// picked from LootTypeEntries and the height over the ground it stops at.
struct FGoosePickupSpawnInfo
{
    UClass* PickupClass;
    FVector Location;
    FRotator Rotation;
    float DeployAltitude;
    // An FName at +0x24 (4-aligned, Core's FName is 8-aligned).
    uint8_t LootTableName[8];

    FName GetLootTableName() const
    {
        FName Name;
        std::memcpy(&Name, LootTableName, sizeof(Name));
        return Name;
    }
};
static_assert(sizeof(FGoosePickupSpawnInfo) == 0x30);
static_assert(offsetof(FGoosePickupSpawnInfo, DeployAltitude) == 0x20);
static_assert(offsetof(FGoosePickupSpawnInfo, LootTableName) == 0x24);

// Air Royale's floating upgrade (8.40; GoosePickup_C). It drops from where it spawns (OnFalling_Server: its
// unreflected height over the ground, then the ground under it, gives DeployHeight), floats there for DeployedLifetime
// and gives the plane that flies into it (OnCollide) its loot table's effect or weapon.
class AAthenaGoosePickup : public AActor
{
    DefineUnrealClass(AAthenaGoosePickup, FortniteGame)
    DefineUFunction(OnRep_DisplayIndex)

public:
    DefineUProperty(int32_t, DisplayIndex)
    DefineUProperty(FName, ForcedLootTableName)
    DefineUStructArrayProperty(FGooseDisplayData, DisplayData)
    DefineUProperty(float, DeployHeight)
    static bool HasSpawnMembers()
    {
        return StaticClass() && FGooseDisplayData::StaticStruct() && Member_DisplayIndex.IsAvailable() && Member_ForcedLootTableName.IsAvailable() && Member_DisplayData.IsAvailable()
            && Member_DeployHeight.IsAvailable() && FindStaticFunction_OnRep_DisplayIndex();
    }

    // The loot table OnCollide gives (8.40 0xEA3EE0 passes +0x774, after DisplayIndex; the constructor 0xE89120 leaves
    // it None) and the height over the ground it stops at (+0x8EC, after DeployHeight; the constructor's 1000, read by
    // OnFalling_Server 0xEA4480). Both unreflected; -1 where the default object doesn't hold the constructor's values.
    static ptrdiff_t LootTableNameOffset();
    static ptrdiff_t DeployAltitudeOffset();

    // The mutator's pickup timer spawns a pickup with this (Core's name; its call at 8.40 0xFF5E94 goes to the shared
    // return-zero function). Deferred, so the pickup's BeginPlay, which starts its fall, has its loot table, height
    // and look: ForcedLootTableName where the class sets one, else Info's table, and the DisplayData entry for it.
    static AAthenaGoosePickup* SpawnGoosePickup(UWorld* World, const FGoosePickupSpawnInfo& Info);

    void OnRep_DisplayIndex();
};
