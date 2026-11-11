#pragma once

#include "GameFramework/Info.h"
#include "GameplayTagContainer.h"
#include "Math/Vector2D.h"

#include <cstddef>

class APlayerState;

// What AFortSpecialActorReplicationInfo::AddActorToReplicationList takes (21.40's FSpecialActorInitData; native, not
// reflected): the actor, its unique id and category, and the icons the map and the compass draw for it. The build's own
// constructor and destructor run on it (5.40 0xECE8A0 and 0xBA5D80, from AFortAthenaExitCraftSpawner::
// RegisterSpawnerSpecialActor), so its brushes, its FText DisplayName and its StatsToTrack array are the build's own
// types, defaulted as Epic's are (RepInterval 0.5, RepIntervalDev 0.1, scales 1). The members are at 5.40's offsets
// (5.41's constructor writes the same ones); only their raw storage is Core's.
struct FSpecialActorInitData
{
    // The build's constructor; only where AFortSpecialActorReplicationInfo::ResolveNatives found it.
    FSpecialActorInitData();
    ~FSpecialActorInitData();
    FSpecialActorInitData(const FSpecialActorInitData&) = delete;
    FSpecialActorInitData& operator=(const FSpecialActorInitData&) = delete;

    AActor* SpecialActorRef;
    // The list's key: "<CategoryTag>_<actor name>" for every native registration (RegisterSpawnerSpecialActor's
    // "%s_%s"), which the registering actor keeps to take itself off again.
    FName UniqueID;
    FGameplayTag CategoryTag;
    APlayerState* PlayerState;
    float RepInterval;
    float RepIntervalDev;
    // FSlateBrush (0x88 bytes, its vtable first): the map's icon, then the compass's.
    alignas(8) uint8_t MinimapIconBrush[0x88];
    FVector2D MinimapScale;
    alignas(8) uint8_t CompassIconBrush[0x88];
    FVector2D CompassScale;
    bool bReplicateHealth;
    bool bReplicateShield;
    // FText (0x18 bytes).
    alignas(8) uint8_t DisplayName[0x18];
    // TArray<FSpecialActorSingleStatData>.
    alignas(8) uint8_t StatsToTrack[0x10];
};
static_assert(offsetof(FSpecialActorInitData, UniqueID) == 0x8);
static_assert(offsetof(FSpecialActorInitData, CategoryTag) == 0x10);
static_assert(offsetof(FSpecialActorInitData, RepInterval) == 0x20);
static_assert(offsetof(FSpecialActorInitData, MinimapIconBrush) == 0x28);
static_assert(offsetof(FSpecialActorInitData, MinimapScale) == 0xB0);
static_assert(offsetof(FSpecialActorInitData, CompassIconBrush) == 0xB8);
static_assert(offsetof(FSpecialActorInitData, CompassScale) == 0x140);
static_assert(offsetof(FSpecialActorInitData, bReplicateHealth) == 0x148);
static_assert(offsetof(FSpecialActorInitData, DisplayName) == 0x150);
static_assert(offsetof(FSpecialActorInitData, StatsToTrack) == 0x168);
static_assert(sizeof(FSpecialActorInitData) == 0x178);

// From 5.40: the game state's list of the actors every client's map and compass show wherever they are (The Getaway's
// jewel drops, carriers, van spawners and vans; AFortGameStateAthena::SpecialActorData). Its SpecialActorRepList is a
// fast array: a client's FSpecialActorRepData::PostReplicatedAdd and PreReplicatedRemove (5.40 0xEE7D40, 0xEE8230)
// broadcast the static OnSpecialActorAdded and OnSpecialActorRemoved the HUD draws the icons from.
class AFortSpecialActorReplicationInfo : public AInfo
{
    DefineUnrealClass(AFortSpecialActorReplicationInfo, FortniteGame)

public:
    // Adds the actor to the list (or finds it there) and returns its index; INDEX_NONE without an actor. 5.40 0xED2470.
    int32_t AddActorToReplicationList(const FSpecialActorInitData& InitData);
    // Takes the actor with that id off the list, so its icons go from every map and compass. 5.40 0xEEB5B0.
    void RemoveActorFromReplicationList(FName UniqueID);

    // The two above and the init data's constructor and destructor (Finder::ResolveSpecialActorReplicationLive); false
    // on builds before 5.40, which have no special actors.
    static bool ResolveNatives();
};
