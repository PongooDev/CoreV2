#include "pch.h"
#include "Building/BuildingContainer.h"
#include "AbilitySystemBlueprintLibrary.h"
#include "Athena/FortGameModeAthena.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/NetDriver.h"
#include "Engine/World.h"
#include "Finder.h"
#include "FortGameState.h"
#include "FortKismetLibrary.h"
#include "FortWorldManager.h"
#include "Hooks.h"
#include "Items/FortItem.h"
#include "Items/FortItemDefinition.h"
#include "Items/FortPickup.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Logging/LogMacros.h"
#include "Math/Rotator.h"
#include "Math/Transform.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Natives.h"
#include "Pawns/FortPawn.h"
#include "Perception/AISense_Hearing.h"
#include "Player/FortPlayerController.h"
#include "Player/FortPlayerState.h"
#include "ReflectionParams.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"

#include <algorithm>
#include <cmath>
#include <vector>

DEFINE_LOG_CATEGORY_STATIC(LogFortBuilding, Log, All);

namespace
{
    template <class TFunction>
    TFunction ContainerSlot(const ABuildingContainer* Container, int32_t Offset)
    {
        return reinterpret_cast<TFunction>((*reinterpret_cast<void* const* const*>(Container))[Natives::ABuildingContainer_SpawnLootIndex + Offset]);
    }

    void ApplyLootedDurability(FFortItemEntry& Entry, UFortWorldItemDefinition* ItemDefinition, float DurabilityModifier)
    {
        const uint8_t ItemType = ItemDefinition->ItemType;
        if (ItemType != EFortItemType::GetWeaponMelee() && ItemType != EFortItemType::GetWeaponRanged())
        {
            return;
        }
        const float Durability = ItemDefinition->GetMaxDurability(Entry.Level) * DurabilityModifier;
        if (Durability != Entry.Durability)
        {
            Entry.Durability = Durability;
            Entry.bIsDirty = true;
        }
        Entry.SetStateValue(EFortItemEntryState::GetDurabilityInitialized(), 1);
    }

    int32_t FloorLootContainers = 0;
    int32_t FloorLootPickups = 0;
    int32_t FloorLootLoadedWeapons = 0;

    // Battle Royale's loot packages have some rows for one loot level only (from 1.8.2: the medium ammo of
    // WorldList.AthenaAmmoLarge and WorldList.AthenaLoot.Ammo(.Medium), MinWorldLevel = MaxWorldLevel = 1, 2 and 3). The
    // loot level is the game state's WorldLevel (UFortKismetLibrary::GetLootLevel reads it too, for the supply drop), which
    // AFortGameStateAthena's constructor sets to 1. 1.9's Per Playlist Loot sets it from the playlist with a new
    // AFortGameState virtual (1.9 slot 0xE5, empty in the base). AFortGameStateAthena's override (0x430920) gives
    // playlist -1 to 2 level 1, 10 (Duo) level 2, and 9 (Squad) and 11 level 3. AFortGameModeAthena::OnGivenMatchAssignment
    // (0x42D2B0) calls it, after it copies the match's playlist to the game state, and so does the world manager when it
    // sets the world level (0x667A50). Before the match is assigned, the floor loot is rolled at level 1 (and again, at
    // the match's level, when the assignment brings other loot tables: RollAthenaLootAgain). Logged when a roll
    // uses a level other than the last one logged, so a server log shows which level each playlist's loot got.
    void LogAthenaLootLevel(ABuildingContainer* Container, int32_t LootLevel)
    {
        static int32_t LastLogged = -2;
        if (LootLevel == LastLogged)
        {
            return;
        }
        LastLogged = LootLevel;
        AFortGameModeAthena* GameMode = AFortGameModeAthena::GetCurrent(Container);
        UE_LOG(LogFortBuilding, Log, TEXT("Battle Royale loot level %d (playlist %d), first rolled for %s"), LootLevel, GameMode ? static_cast<int32_t>(GameMode->CurrentPlaylistId) : -1, *Container->GetName());
    }
}

bool ABuildingContainer::ServerOnAttemptInteract(AFortPawn* InteractingPawn)
{
#if WITH_SERVER_CODE
    if (!InteractingPawn || bDestroyed || (Natives::ABuildingContainer_SpawnLootIndex < 0 && !Finder::ResolveBuildingLootLive()) || IsAlreadySearched(InteractingPawn))
    {
        return false;
    }
    return ContainerSlot<bool (*)(ABuildingContainer*, AFortPawn*)>(this, 0)(this, InteractingPawn);
#else
    return false;
#endif
}

bool ABuildingContainer::IsAlreadySearched(AFortPawn* Looter)
{
    // Three slots before SpawnLoot up to 3.3, four from 3.5 (Finder::ResolveBuildingLootLive).
    return ContainerSlot<bool (*)(ABuildingContainer*, AFortPawn*)>(this, Natives::ABuildingContainer_IsAlreadySearchedIndex - Natives::ABuildingContainer_SpawnLootIndex)(this, Looter);
}

void ABuildingContainer::ModifyLootDrops(TStructArray<FFortItemEntry>& LootDrops, AFortPlayerController* Looter)
{
    ContainerSlot<void (*)(ABuildingContainer*, TStructArray<FFortItemEntry>&, AFortPlayerController*)>(this, 1)(this, LootDrops, Looter);
}

void ABuildingContainer::OnPickupSpawn(AFortPickup* Pickup, AFortPlayerController* Looter)
{
    ContainerSlot<void (*)(ABuildingContainer*, AFortPickup*, AFortPlayerController*)>(this, 2)(this, Pickup, Looter);
}

void ABuildingContainer::HideContainer()
{
    // Three slots after SpawnLoot, five from 3.2 (Finder::ResolveBuildingLootLive).
    ContainerSlot<void (*)(ABuildingContainer*)>(this, Natives::ABuildingContainer_HideContainerIndex - Natives::ABuildingContainer_SpawnLootIndex)(this);
}

void ABuildingContainer::SetSearched(AFortPawn* Searcher)
{
    ContainerSlot<void (*)(ABuildingContainer*, AFortPawn*)>(this, Natives::ABuildingContainer_HideContainerIndex - Natives::ABuildingContainer_SpawnLootIndex + 1)(this, Searcher);
}

void ABuildingContainer::BounceContainer()
{
    Reflection::FParams Params(StaticFunction_BounceContainer());
    Params.Call(this);
}

void ABuildingContainer::UpdateReplicatedLootTier()
{
    // 1.2 replicates the tier instead of the key (GetLootTier returns ReplicatedLootTier); Cert has no such member.
    static const int32_t Offset = []
    {
        const UProperty* Property = StaticClass() ? StaticClass()->FindPropertyByName(FName(L"ReplicatedLootTier", FNAME_Find)) : nullptr;
        return Property ? Property->GetOffset_ForInternal() : -1;
    }();
    if (Offset >= 0)
    {
        const FName LootTierKey = ContainerLootTierKey;
        *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(this) + Offset) = LootTierKey.IsNone() ? -1 : UFortLootTier::FindLootTierFromKey(LootTierKey);
    }
}

FName ABuildingContainer::GetSearchLootTierGroup()
{
    const FName TierGroup = SearchLootTierGroup;
    AFortGameModeAthena* AthenaGameMode = AFortGameModeAthena::GetCurrent(this);
    return AthenaGameMode ? AthenaGameMode->RedirectLootTierGroup(TierGroup) : TierGroup;
}

bool ABuildingContainer::StartsAlreadySearched()
{
    // Nothing in 1.7.2 reads bStartAlreadySearched_Athena (+0x1078 bit 6) any more; only the constructor (0x457940) sets
    // the toss defaults.
    return Member_bStartAlreadySearched_Athena.IsAvailable() && bStartAlreadySearched_Athena && AFortGameModeAthena::GetCurrent(this);
}

FVector ABuildingContainer::GetAthenaTossForward()
{
    // LootTossDirection_Athena's yaw on the ground, relative to the container (the constructor's 90: local +Y, the side
    // LootSpawnLocation is on).
    constexpr float DegreesToRadians = 3.1415926535897932f / 180.0f;
    const float Yaw = static_cast<FRotator>(LootTossDirection_Athena).Yaw * DegreesToRadians;
    FVector Forward = GetTransform().Rotation.RotateVector(FVector(std::cos(Yaw), std::sin(Yaw), 0.0f));
    Forward.Z = 0.0f;
    const float ForwardSize = std::sqrt(Forward.X * Forward.X + Forward.Y * Forward.Y);
    return ForwardSize > 0.0f ? Forward * (1.0f / ForwardSize) : FVector(1.0f, 0.0f, 0.0f);
}

namespace
{
    // A floor loot container's spot, kept after the container is gone, with the pickups its roll spawned, so the roll
    // can be made again with the match's loot tables (RollAthenaLootAgain).
    struct FFloorLootSpot
    {
        FWeakObjectPtr World;
        FVector Base;
        FVector Forward;
        float Height = 0.0f;
        FName TierGroup;
        FGameplayTagContainer Tags;
        float DurabilityModifier = 1.0f;
        std::vector<FWeakObjectPtr> Pickups;
    };

    std::vector<FFloorLootSpot> FloorLootSpots;

    void GetFloorLootLocations(const FFloorLootSpot& Spot, int32_t Index, int32_t Count, FVector& OutSpawnLocation, FVector& OutFinalLocation)
    {
        // Floor loot (Tiered_Athena_FloorLoot_01) is out before anyone lands, so it is only dropped into place (#72): each
        // item gets its own spot around the container's pivot (its floor) and drops from LootSpawnLocation's height a little
        // short of it, never straight above it: AFortPickup::TossPickup (1.7.2 0x6622F0) sends a toss with no distance in a
        // random direction 512 away.
        constexpr float FloorLootRadius = 40.0f;
        constexpr float DropShortfall = 20.0f;
        const FVector Right(-Spot.Forward.Y, Spot.Forward.X, 0.0f);
        const float Angle = Count > 1 ? 2.0f * 3.1415926535897932f * static_cast<float>(Index) / static_cast<float>(Count) : 0.0f;
        const FVector Direction = Spot.Forward * std::cos(Angle) + Right * std::sin(Angle);
        OutFinalLocation = Spot.Base + Direction * (Count > 1 ? FloorLootRadius : DropShortfall);
        OutSpawnLocation = OutFinalLocation - Direction * DropShortfall + FVector(0.0f, 0.0f, Spot.Height);
    }

    void SpawnFloorLootAt(FFloorLootSpot& Spot, UWorld* World, int32_t WorldLevel)
    {
        FName LootTierKey;
        int32_t LootTier = -1;
        UFortKismetLibrary::PickLootTierKeyAndLootTierFromTierGroup(LootTierKey, LootTier, Spot.TierGroup, WorldLevel, -1, Spot.Tags);
        ++FloorLootContainers;
        if (!UFortLootTier::ShouldDropLoot(LootTierKey, L"ABuildingContainer::PostUpdate"))
        {
            return;
        }

        TStructArray<FFortItemEntry> LootDrops{};
        UFortLootPackage::PickLootDrops(LootDrops, WorldLevel, LootTierKey, nullptr, Spot.Tags, true, false);
        AFortPickup::CombineItemEntries(LootDrops, nullptr);
        const FRotator SpawnRotation(0.0f, 0.0f, 0.0f);
        for (int32_t Index = 0; Index < LootDrops.Num(); ++Index)
        {
            FFortItemEntry& Entry = LootDrops[Index];
            UFortWorldItemDefinition* WorldItemDefinition = Cast<UFortWorldItemDefinition>(static_cast<UFortItemDefinition*>(Entry.ItemDefinition));
            if (!WorldItemDefinition)
            {
                continue;
            }
            ApplyLootedDurability(Entry, WorldItemDefinition, Spot.DurabilityModifier);
            FloorLootLoadedWeapons += Entry.LoadFullClip() ? 1 : 0;
            FVector SpawnLocation;
            FVector FinalLocation;
            GetFloorLootLocations(Spot, Index, LootDrops.Num(), SpawnLocation, FinalLocation);
            if (AFortPickup* Pickup = AFortPickup::CreatePickup(World, Entry, &SpawnLocation, &SpawnRotation))
            {
                Pickup->TossPickup(FinalLocation, nullptr, 0);
                Spot.Pickups.emplace_back(Pickup);
                ++FloorLootPickups;
            }
        }
        LootDrops.Empty();
    }
}

float ABuildingContainer::GetAthenaLootFront()
{
    // How far along GetAthenaTossForward the front of the container's collision is. The actor's bounds are a box on the
    // world's axes, and most containers are turned some way (386 of 553 on Athena_Terrain), so for a chest turned 45
    // degrees the box's front was 40 past the real one, 30 past the pivot (Tiered_Chest_6_Parent's mesh is 84 x 69). The
    // front is the middle of those bounds plus half the depth of the mesh's own box (GetLocalBounds) along the toss.
    constexpr float DegreesToRadians = 3.1415926535897932f / 180.0f;
    FVector Origin;
    FVector Extent;
    GetActorBounds(true, Origin, Extent);
    const FVector Forward = GetAthenaTossForward();
    FVector Min;
    FVector Max;
    UStaticMeshComponent* Mesh = StaticMeshComponent;
    if (!Mesh || !Mesh->GetLocalBounds(Min, Max))
    {
        return (Origin | Forward) + FMath::Abs(Forward.X) * Extent.X + FMath::Abs(Forward.Y) * Extent.Y;
    }
    const float Yaw = static_cast<FRotator>(LootTossDirection_Athena).Yaw * DegreesToRadians;
    const FVector Scale = GetTransform().Scale3D;
    const float Depth = FMath::Abs(std::cos(Yaw) * Scale.X) * (Max.X - Min.X) + FMath::Abs(std::sin(Yaw) * Scale.Y) * (Max.Y - Min.Y);
    return (Origin | Forward) + Depth * 0.5f;
}

FVector ABuildingContainer::GetAthenaLootSpawnLocation()
{
    // Battle Royale's chests and ammo boxes pop their loot out from the edge of their collision (#90). LootSpawnLocation
    // is no longer the container's own point by then: SetMeshSet (1.7.2 0x45F840, called by SelectMeshSet 0x47E2A0)
    // copies the chosen mesh set's over it, and every tiered mesh set keeps the FMeshSet default, 100 over the pivot. Loot
    // started there, 31 above a chest's lid (Tiered_Chest_6_Parent's mesh is 69 high) and 51 above an ammo box's (49).
    // The class's own point (its default object's) is the one its blueprint sets: a chest's (0, 42, 72) is just past its
    // front (local +Y, 30 from the pivot) at the top of the lid, an ammo box's (Tiered_Short_Ammo_3_Parent) (0, 0, 32)
    // is inside it. The loot leaves from that point, moved out to just past the front of the container's collision, and
    // no lower than where the pickup's own collision (a capsule, radius 30, half height 80: AFortPickup's constructor,
    // 1.7.2 0x65BA90, 1.8 0x677F80) would sit on the container's floor, as its toss can't start inside the floor. For
    // both that is 80 over the pivot (where the pickup comes to rest on the floor in front), 11 above a chest's lid.
    constexpr float PickupRadius = 30.0f;
    constexpr float PickupHalfHeight = 80.0f;
    ABuildingContainer* Default = static_cast<ABuildingContainer*>(GetClass()->GetDefaultObject());
    const FVector LocalLocation = Default && Default != this ? static_cast<FVector>(Default->LootSpawnLocation) : static_cast<FVector>(LootSpawnLocation);
    const FTransform ContainerTransform = GetTransform();
    FVector Location = ContainerTransform.TransformPosition(LocalLocation);
    const FVector Forward = GetAthenaTossForward();
    Location = Location + Forward * std::max(GetAthenaLootFront() + PickupRadius - (Location | Forward), 0.0f);
    Location.Z = std::max(Location.Z, ContainerTransform.GetLocation().Z + PickupHalfHeight);
    return Location;
}

FVector ABuildingContainer::GetAthenaLootTossVelocity()
{
    // The container's own toss, from the properties Battle Royale added for it (1.7.2 on; the constructor, 2.4 0x47B620,
    // sets LootTossDirection_Athena (pitch 65, yaw 90: up and out over local +Y, the side LootSpawnLocation is on),
    // LootTossSpeed_Athena 550 and LootTossConeHalfAngle_Athena 60). The search that read them is server code no client
    // build has: in 2.4 nothing but the constructor touches them. Each item leaves at LootTossSpeed_Athena in a random
    // direction within LootTossConeHalfAngle_Athena of LootTossDirection_Athena, turned with the container, and its
    // bounces decide where it lands (#203).
    constexpr float DegreesToRadians = 3.1415926535897932f / 180.0f;
    const FRotator TossRotation = LootTossDirection_Athena;
    const float Pitch = TossRotation.Pitch * DegreesToRadians;
    const float Yaw = TossRotation.Yaw * DegreesToRadians;
    const FVector LocalDirection(std::cos(Pitch) * std::cos(Yaw), std::cos(Pitch) * std::sin(Yaw), std::sin(Pitch));
    const FVector TossDirection = GetTransform().Rotation.RotateVector(LocalDirection);
    const float ConeHalfAngle = FMath::Clamp(static_cast<float>(LootTossConeHalfAngle_Athena), 0.0f, 180.0f) * DegreesToRadians;
    return UKismetMathLibrary::RandomUnitVectorInCone(TossDirection, ConeHalfAngle) * std::max(static_cast<float>(LootTossSpeed_Athena), 0.0f);
}

FVector ABuildingContainer::GetAthenaLootFinalLocation()
{
    // Where the toss is headed, for the pickup's LootFinalPosition: the container's LootFinalLocation (the constructor's
    // (0, 100, 0): on its floor, 100 out over local +Y). The launch decides where the item really lands.
    const FTransform ContainerTransform = GetTransform();
    if (Member_LootFinalLocation.IsAvailable())
    {
        return ContainerTransform.TransformPosition(static_cast<FVector>(LootFinalLocation));
    }
    return ContainerTransform.GetLocation() + GetAthenaTossForward() * 100.0f;
}

void ABuildingContainer::SpawnFloorLoot()
{
    // Battle Royale's floor loot (Tiered_Athena_FloorLoot_01: Loot_AthenaFloorLoot, bStartAlreadySearched_Athena,
    // bDestroyContainerOnSearch) is never searched: its roll lies on the ground from the start, without the container.
    UWorld* World = GetWorld();
    const int32_t WorldLevel = GetWorldManager() ? GetWorldLevel() : -1;
    LogAthenaLootLevel(this, WorldLevel);

    // The spots of an earlier world are gone with it.
    std::erase_if(FloorLootSpots, [World](const FFloorLootSpot& Spot) { return Spot.World.Get() != World; });
    const FTransform ContainerTransform = GetTransform();
    FFloorLootSpot& Spot = FloorLootSpots.emplace_back();
    Spot.World = World;
    Spot.Base = ContainerTransform.GetLocation();
    Spot.Forward = GetAthenaTossForward();
    Spot.Height = FMath::Max(ContainerTransform.TransformPosition(LootSpawnLocation).Z - Spot.Base.Z, 30.0f);
    Spot.TierGroup = GetSearchLootTierGroup();
    Spot.Tags.AppendTags(StaticGameplayTags);
    Spot.DurabilityModifier = LootedWeaponsDurabilityModifier;
    SpawnFloorLootAt(Spot, World, WorldLevel);
}

void ABuildingContainer::RollAthenaLootAgain(UWorld* World)
{
#if WITH_SERVER_CODE
    // AFortGameModeAthena::ApplyPlaylistDataTables copies the playlist's rows over the default tables when the match is
    // assigned, after the world rolled its chests' and ammo boxes' tiers and spawned its floor loot (at its creation, from
    // the tables the map URL's playlist had). What is rolled when searched (chests, ammo boxes, supply drops) reads the
    // tables then; the rest is rolled again here: each container's tier, and the floor loot, its earlier pickups removed.
    AFortGameState* GameState = World ? Cast<AFortGameState>(reinterpret_cast<UObject*>(World->GetGameState())) : nullptr;
    const int32_t WorldLevel = GameState && GameState->WorldManager ? static_cast<int32_t>(GameState->WorldLevel) : -1;

    int32_t Containers = 0;
    for (TObjectIterator<ABuildingContainer> It; It; ++It)
    {
        ABuildingContainer* Container = *It;
        if (Container->GetWorld() != World || Container->HasAnyFlags(static_cast<EObjectFlags>(RF_ClassDefaultObject | RF_ArchetypeObject)) || Container->bDestroyed
            || static_cast<FName>(Container->ContainerLootTierKey).IsNone() || Container->LootTestingData)
        {
            continue;
        }
        FName PickedKey;
        int32_t LootTier = -1;
        UFortKismetLibrary::PickLootTierKeyAndLootTierFromTierGroup(PickedKey, LootTier, Container->GetSearchLootTierGroup(), WorldLevel, -1, Container->StaticGameplayTags);
        Container->ContainerLootTierKey = PickedKey;
        if (!PickedKey.IsNone())
        {
            Container->SelectMeshSet(LootTier);
        }
        Container->UpdateReplicatedLootTier();
        ++Containers;
    }

    FloorLootContainers = 0;
    FloorLootPickups = 0;
    FloorLootLoadedWeapons = 0;
    int32_t Removed = 0;
    for (FFloorLootSpot& Spot : FloorLootSpots)
    {
        if (Spot.World.Get() != World)
        {
            continue;
        }
        for (const FWeakObjectPtr& Pickup : Spot.Pickups)
        {
            if (AActor* Actor = static_cast<AActor*>(Pickup.Get()))
            {
                Actor->K2_DestroyActor();
                ++Removed;
            }
        }
        Spot.Pickups.clear();
        SpawnFloorLootAt(Spot, World, WorldLevel);
    }
    UE_LOG(LogFortBuilding, Log, TEXT("Loot rolled again (loot level %d): %d containers' tiers; floor loot: %d pickups removed, %d containers spawned %d pickups (%d loaded weapons)"),
        WorldLevel, Containers, Removed, FloorLootContainers, FloorLootPickups, FloorLootLoadedWeapons);    FloorLootContainers = 0;
    FloorLootPickups = 0;
    FloorLootLoadedWeapons = 0;
#endif
}

void ABuildingContainer::PostLoad()
{
    PostLoadOG(this);
#if WITH_SERVER_CODE
    if (HasAnyFlags(static_cast<EObjectFlags>(RF_ClassDefaultObject | RF_ArchetypeObject)))
    {
        return;
    }
    const FName LootTierKey = ContainerLootTierKey;
    if (!LootTierKey.IsNone())
    {
        SelectMeshSet(LootTierKey);
    }
    UpdateReplicatedLootTier();
#endif
}

float& ABuildingContainer::SavedReservedRandomValue()
{
    // Up to 3.1 the container has no SavedReservedRandomValueResult: Core keeps the value for the one call.
    static float Saved = 0.0f;
    return Member_SavedReservedRandomValueResult.IsAvailable() ? GetMember_SavedReservedRandomValueResult() : Saved;
}

void ABuildingContainer::PostUpdate(EFortBuildingPersistentState State, FFortBuildingRandomList* RandomList)
{
#if WITH_SERVER_CODE
    // As 3.2 (0xA364A0) does it. Up to 3.1 the roll was in PostUpdate itself.
    ABuildingSMActor::PostUpdate(State, RandomList);
    SavedReservedRandomValue() = (RandomList ? RandomList->Next() : 0.0f) * 1.0f + 0.00000001f;
    InitializeTierGroupData(State, false);
#endif
}

void ABuildingContainer::InitializeTierGroupData(EFortBuildingPersistentState State, bool bInitializeAgain)
{
#if WITH_SERVER_CODE
    // The loot tables are the match's playlist's from the world's first roll on.
    if (AFortGameModeAthena* AthenaGameMode = AFortGameModeAthena::GetCurrent(this))
    {
        AthenaGameMode->ApplyPlaylistDataTablesOnce();
    }

    if (StartsAlreadySearched())
    {
        if (State == EFortBuildingPersistentState::GetNew())
        {
            SpawnFloorLoot();
        }
        ContainerLootTierKey = FName();
        SetSearched(nullptr);
        AFortWorldManager* WorldManager = GetWorldManager();
        if (bPersistToWorld && WorldManager)
        {
            UpdateActorRecord(WorldManager, EFortBuildingPersistentState::GetSearched(), false);
        }
        // With bDestroyContainerOnSearch set, HideContainer takes its destroy branch.
        HideContainer();
        return;
    }

    // Battle Royale rolls its own groups (Loot_Treasure -> Loot_AthenaTreasure, Loot_Ammo -> Loot_AthenaAmmoLarge). With
    // Save the World's, the roll gave no loot tier in a Battle Royale match and every chest and ammo box started searched.
    const FName TierGroup = GetSearchLootTierGroup();
    const bool bAthena = AFortGameModeAthena::GetCurrent(this) != nullptr;
    if (State == EFortBuildingPersistentState::GetNew())
    {
        float ShowChance = 0.0f;
        const bool bSpawnAllLoot = FParse::Param(FCommandLine::Get(), L"SpawnAllLoot");
        if (bSpawnAllLoot)
        {
            ShowChance = 1.0f;
        }

        if (!static_cast<FName>(SearchLootTierGroup).IsNone())
        {
            ShowChance = 1.0f;
            AFortWorldManager* WorldManager = GetWorldManager();
            int32_t LootTier = -1;
            // The world loot quota picks from Save the World's groups.
            const FChosenQuotaInfo& ChosenQuota = SearchLootTierChosenQuotaInfo;
            if (ChosenQuota.IsSet() && !bAthena)
            {
                ContainerLootTierKey = ChosenQuota.LootTierKey;
                LootTier = ChosenQuota.LootTier;
            }
            else
            {
                FName PickedKey = ContainerLootTierKey;
                UFortKismetLibrary::PickLootTierKeyAndLootTierFromTierGroup(PickedKey, LootTier, TierGroup, WorldManager ? GetWorldLevel() : -1, -1, StaticGameplayTags);
                ContainerLootTierKey = PickedKey;
            }
            SelectMeshSet(LootTier);

            EFortBuildingPersistentState NewState = EFortBuildingPersistentState::GetDefault();
            if (!bSpawnAllLoot && !bAlwaysMaintainLoot && !UFortLootTier::ShouldDropLoot(ContainerLootTierKey, L"ABuildingContainer::PostUpdate"))
            {
                ContainerLootTierKey = FName();
                NewState = EFortBuildingPersistentState::GetSearched();
                SetSearched(nullptr);
                if (!bAlwaysShowContainer)
                {
                    ShowChance = 0.0f;
                }
            }

            if (bPersistToWorld && WorldManager)
            {
                if (AltMeshIdx != -1)
                {
                    UpdateActorRecord(WorldManager, NewState, true);
                }
                else if (NewState == EFortBuildingPersistentState::GetSearched())
                {
                    UpdateActorRecord(WorldManager, NewState, false);
                }
            }
        }

        if (SavedReservedRandomValue() > ShowChance)
        {
            HideContainer();
            ContainerLootTierKey = FName();
        }
        UE_LOG(LogFortBuilding, Verbose, TEXT("ABuildingContainer::PostUpdate() on new container Container: %s, ContainerLootTierKey: %s, AltMeshIdx: %d"), *GetName(), *static_cast<FName>(ContainerLootTierKey).ToString(), static_cast<int32_t>(AltMeshIdx));
    }
    else
    {
        const FName LootTierKey = ContainerLootTierKey;
        if (!LootTierKey.IsNone())
        {
            SelectMeshSet(LootTierKey);
        }
        else
        {
            int32_t LootTier = -1;
            FName PickedKey = ContainerLootTierKey;
            UFortKismetLibrary::PickLootTierKeyAndLootTierFromTierGroup(PickedKey, LootTier, TierGroup, GetWorldLevel(), -1, StaticGameplayTags);
            ContainerLootTierKey = PickedKey;
            SelectMeshSet(LootTier);
        }
        if (State == EFortBuildingPersistentState::GetSearched())
        {
            SetSearched(nullptr);
        }
        UE_LOG(LogFortBuilding, Verbose, TEXT("ABuildingContainer::PostUpdate() Container: %s, ContainerLootTierKey: %s, AltMeshIdx: %d"), *GetName(), *static_cast<FName>(ContainerLootTierKey).ToString(), static_cast<int32_t>(AltMeshIdx));
    }
    UpdateReplicatedLootTier();
#endif
}

bool ABuildingContainer::SpawnLoot(AFortPawn* Looter)
{
#if WITH_SERVER_CODE
    // A destroyed container (Battle Royale's chests and ammo boxes the match doesn't spawn, #205) has nothing to search.
    if (Role < ROLE_Authority || !Looter || bDestroyed)
    {
        return false;
    }

    bool bSpawnedLoot = false;
    ForceNetUpdate();
    const FTransform ContainerTransform = GetTransform();
    const bool bAthena = AFortGameModeAthena::GetCurrent(this) != nullptr;
    if (!IsAlreadySearched(Looter))
    {
        AFortWorldManager* WorldManager = GetWorldManager();
        // A Battle Royale container whose tier was never rolled (no PostUpdate) rolls its redirected group now.
        if (bAthena && !LootTestingData && static_cast<FName>(ContainerLootTierKey).IsNone())
        {
            FName PickedKey;
            int32_t LootTier = -1;
            UFortKismetLibrary::PickLootTierKeyAndLootTierFromTierGroup(PickedKey, LootTier, GetSearchLootTierGroup(), WorldManager ? GetWorldLevel() : -1, -1, StaticGameplayTags);
            ContainerLootTierKey = PickedKey;
        }
        const FVector SpawnLocation = bAthena ? GetAthenaLootSpawnLocation() : ContainerTransform.TransformPosition(LootSpawnLocation);
        const FRotator SpawnRotation(0.0f, 0.0f, 0.0f);
        UWorld* World = GetWorld();
        UScriptStruct* EntryStruct = FFortItemEntry::StaticStruct();

        if (UFortWorldItemDefinition* TestingData = LootTestingData)
        {
            std::vector<uint8_t> EntryStorage(EntryStruct->GetStructureSize(), 0);
            FFortItemEntry* Entry = reinterpret_cast<FFortItemEntry*>(EntryStorage.data());
            FFortItemEntry::Construct(Entry, TestingData, 1, 0);
            ApplyLootedDurability(*Entry, TestingData, LootedWeaponsDurabilityModifier);
            if (bAthena)
            {
                Entry->LoadFullClip();
            }
            if (AFortPickup* Pickup = AFortPickup::CreatePickup(World, *Entry, &SpawnLocation, &SpawnRotation))
            {
                Pickup->SetPickupTarget(Looter, -1.0f, FVector(0.0f, 0.0f, 1.0f), true);
                bSpawnedLoot = true;
            }
            EntryStruct->DestroyStruct(Entry);
        }
        else if (!static_cast<FName>(ContainerLootTierKey).IsNone())
        {
            TStructArray<FFortItemEntry> LootDrops{};
            const int32_t LootLevel = WorldManager ? GetWorldLevel() : -1;
            if (bAthena)
            {
                LogAthenaLootLevel(this, LootLevel);
            }
            UFortLootPackage::PickLootDrops(LootDrops, LootLevel, ContainerLootTierKey, UAbilitySystemBlueprintLibrary::GetAbilitySystemComponent(Looter), StaticGameplayTags, true, false);

            AFortPlayerController* LooterController = Cast<AFortPlayerController>(reinterpret_cast<UObject*>(static_cast<AController*>(Looter->Controller)));
            ModifyLootDrops(LootDrops, LooterController);
            AFortPickup::CombineItemEntries(LootDrops, LooterController ? static_cast<AFortInventory*>(LooterController->WorldInventory) : nullptr);
            const FVector FinalLocation = bAthena ? GetAthenaLootFinalLocation() : FVector(0.0f, 0.0f, 0.0f);
            HighestRarity = 0;
            for (int32_t Index = 0; Index < LootDrops.Num(); ++Index)
            {
                FFortItemEntry& Entry = LootDrops[Index];
                UFortItemDefinition* ItemDefinition = Entry.ItemDefinition;
                UFortWorldItemDefinition* WorldItemDefinition = Cast<UFortWorldItemDefinition>(ItemDefinition);
                if (!WorldItemDefinition)
                {
                    UE_LOG(LogFortBuilding, Warning, TEXT("Attempted to spawn non-world item %s!"), ItemDefinition ? *ItemDefinition->GetName() : TEXT("None"));
                    continue;
                }

                HighestRarity = FMath::Max(static_cast<uint8_t>(HighestRarity), WorldItemDefinition->GetRarity());
                ApplyLootedDurability(Entry, WorldItemDefinition, LootedWeaponsDurabilityModifier);
                if (bAthena)
                {
                    // Battle Royale's looted weapons come loaded (#85); the ammo for them is the loot package's own drop.
                    Entry.LoadFullClip();
                }
                if (AFortPickup* Pickup = AFortPickup::CreatePickup(World, Entry, &SpawnLocation, &SpawnRotation))
                {
                    if (bAthena)
                    {
                        Pickup->TossFromContainer(FinalLocation, GetAthenaLootTossVelocity());
                    }
                    else
                    {
                        const FVector Direction = UKismetMathLibrary::RandomUnitVectorInCone(FVector(0.0f, 0.0f, 1.0f), 0.2617994f);
                        const float FlyTime = AFortPickup::GetRandomFlyTime() + static_cast<float>(Index) * 0.3f;
                        Pickup->SetPickupTarget(Looter, FlyTime, Direction, true);
                    }
                    OnPickupSpawn(Pickup, LooterController);
                    bSpawnedLoot = true;
                }
            }

            LootDrops.Empty();
        }

        if (AFortPlayerStateZone* PlayerState = Cast<AFortPlayerStateZone>(reinterpret_cast<UObject*>(static_cast<APlayerState*>(Looter->PlayerState))))
        {
            PlayerState->ModifyStat(EFortReplicatedStat::GetContainersLooted(), 1, 1, ContainerTransform.GetLocation());
        }

        const float NoiseRange = LootNoiseRange;
        if (NoiseRange > 0.0f)
        {
            UAISense_Hearing::ReportNoiseEvent(Looter, ContainerTransform.GetLocation(), 1.0f, Looter, NoiseRange, FName());
        }

        if (!bDestroyed)
        {
            SetSearched(Looter);
            if (bPersistToWorld && WorldManager)
            {
                UpdateActorRecord(WorldManager, EFortBuildingPersistentState::GetSearched(), false);
            }
            // With bDestroyContainerOnSearch set, HideContainer takes its destroy branch.
            if (bDestroyContainerOnSearch)
            {
                HideContainer();
            }
        }
    }

    const FVector LooterLocation = Looter->GetTransform().GetLocation();
    const FVector ContainerLocation = ContainerTransform.GetLocation();
    FVector BounceNormal(LooterLocation.X - ContainerLocation.X, LooterLocation.Y - ContainerLocation.Y, 0.0f);
    const float SizeSquared = BounceNormal.SizeSquared();
    if (SizeSquared > 0.00000001f)
    {
        BounceNormal = BounceNormal * FMath::InvSqrt(SizeSquared);
    }
    FFortSearchBounceData& Bounce = SearchBounceData;
    Bounce.BounceNormal = BounceNormal;
    Bounce.SearchAnimationCount = Bounce.SearchAnimationCount + 1;
    BounceContainer();
    return bSpawnedLoot;
#else
    return false;
#endif
}

void ABuildingContainer::HookVTables()
{
    if (Natives::ABuildingContainer_SpawnLootIndex < 0 && !Finder::ResolveBuildingLootLive())
    {
        return;
    }

    const int32_t SpawnLootIndex = Natives::ABuildingContainer_SpawnLootIndex;
    const int32_t PostUpdateIndex = Natives::ABuildingSMActor_PostUpdateIndex;
    const int32_t PostLoadIndex = Natives::ABuildingContainer_PostLoadIndex;
    const uintptr_t StrippedPostUpdate = Natives::ABuildingSMActor_bPostUpdateStripped ? Natives::ABuildingSMActor_PostUpdate : 0;
    const uintptr_t StrippedPostLoad = Natives::ABuildingContainer_bPostLoadStripped ? Natives::ABuildingContainer_PostLoad : 0;
    const int32_t InitializeTierGroupDataIndex = Natives::ABuildingContainer_InitializeTierGroupDataIndex;
    std::vector<uintptr_t> RedirectedPostUpdates;
    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Class = *It;
        UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
        void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
        if (!VTable)
        {
            continue;
        }

        // 1.2 stubs SpawnLoot (return false) and turns the PostUpdate override into a jump to the logging-only parent.
        // 2.4.2's stub still calls GetWorld when it has authority (Finder::IsGetWorldStub).
        const uintptr_t SpawnLoot = reinterpret_cast<uintptr_t>(VTable[SpawnLootIndex]);
        if (Finder::IsStubFunction(SpawnLoot) || Finder::IsGetWorldStub(SpawnLoot, 0))
        {
            Hooks::SwapVTable(L"ABuildingContainer::SpawnLoot", VTable, SpawnLootIndex, reinterpret_cast<void*>(&SpawnLootHK));
        }
        // AKeepItemCoreContainer's override tail-jumps to the stubbed parent.
        else if (const uintptr_t SpawnLootEnd = SpawnLoot >= Finder::TextBegin() && SpawnLoot < Finder::TextEnd() ? Finder::FunctionEnd(SpawnLoot) : 0; SpawnLootEnd > SpawnLoot + 5 && *reinterpret_cast<const uint8_t*>(SpawnLootEnd - 5) == 0xE9
                 && (Finder::IsStubFunction(Finder::RelativeTarget(SpawnLootEnd - 5, 1, 5)) || Finder::IsGetWorldStub(Finder::RelativeTarget(SpawnLootEnd - 5, 1, 5), 0)))
        {
            Hooks::RedirectCall(L"ABuildingContainer::SpawnLoot (Super)", reinterpret_cast<uint8_t*>(SpawnLootEnd - 5), reinterpret_cast<void*>(&SpawnLootHK));
        }

        const uintptr_t PostUpdate = reinterpret_cast<uintptr_t>(VTable[PostUpdateIndex]);
        // 2.4.2's override calls the stripped parent, then GetWorld (Finder::IsGetWorldStub).
        const bool bJumpsToStripped = StrippedPostUpdate && *reinterpret_cast<const uint8_t*>(PostUpdate) == 0xE9 && Finder::RelativeTarget(PostUpdate, 1, 5) == StrippedPostUpdate;
        if (StrippedPostUpdate && (PostUpdate == StrippedPostUpdate || bJumpsToStripped || Finder::IsGetWorldStub(PostUpdate, StrippedPostUpdate)))
        {
            Hooks::SwapVTable(L"ABuildingContainer::PostUpdate", VTable, PostUpdateIndex, reinterpret_cast<void*>(&PostUpdateHK));
        }
        // 3.2 keeps PostUpdate (Finder::ResolveBuildingLootLive) and strips InitializeTierGroupData, where the tier and
        // mesh are rolled: with neither, every container kept its placeholder mesh and no loot tier (#239, #240). The
        // kept PostUpdate's Super call goes to the stripped ABuildingSMActor::PostUpdate, as the other buildings' do.
        else if (InitializeTierGroupDataIndex >= 0)
        {
            // 3.2's stub (0xA2A550) is mov rax, [rcx]; jmp [rax+GetWorld]: a leaf with no unwind entry, so
            // Finder::IsGetWorldStub (which needs the function's end) does not see it.
            const uintptr_t InitializeTierGroupData = reinterpret_cast<uintptr_t>(VTable[InitializeTierGroupDataIndex]);
            const int32_t GetWorld = Natives::UObject_GetWorldIndex >= 0 || Finder::ResolveObjectGetWorld() ? Natives::UObject_GetWorldIndex * static_cast<int32_t>(sizeof(void*)) : -1;
            const bool bGetWorldTail = GetWorld > 0
                && Finder::BodyIs(InitializeTierGroupData, { 0x48, 0x8B, 0x01, 0x48, 0xFF, 0xA0, static_cast<uint8_t>(GetWorld), static_cast<uint8_t>(GetWorld >> 8), static_cast<uint8_t>(GetWorld >> 16), static_cast<uint8_t>(GetWorld >> 24) });
            if (bGetWorldTail || Finder::IsStubFunction(InitializeTierGroupData) || Finder::IsGetWorldStub(InitializeTierGroupData, 0))
            {
                Hooks::SwapVTable(L"ABuildingContainer::InitializeTierGroupData", VTable, InitializeTierGroupDataIndex, reinterpret_cast<void*>(&InitializeTierGroupDataHK));
            }
            const uintptr_t PostUpdateEnd = StrippedPostUpdate && PostUpdate >= Finder::TextBegin() && PostUpdate < Finder::TextEnd() ? Finder::FunctionEnd(PostUpdate) : 0;
            if (PostUpdateEnd && std::find(RedirectedPostUpdates.begin(), RedirectedPostUpdates.end(), PostUpdate) == RedirectedPostUpdates.end())
            {
                RedirectedPostUpdates.push_back(PostUpdate);
                for (const auto& [Site, Target] : Finder::DirectCalls(PostUpdate, PostUpdateEnd))
                {
                    if (Target == StrippedPostUpdate)
                    {
                        Hooks::RedirectCall(L"ABuildingContainer::PostUpdate (Super)", reinterpret_cast<uint8_t*>(Site), reinterpret_cast<void*>(&PostUpdateSuperHK));
                    }
                }
            }
        }

        if (StrippedPostLoad && reinterpret_cast<uintptr_t>(VTable[PostLoadIndex]) == StrippedPostLoad)
        {
            PostLoadOG = reinterpret_cast<decltype(PostLoadOG)>(StrippedPostLoad);
            Hooks::SwapVTable(L"ABuildingContainer::PostLoad", VTable, PostLoadIndex, reinterpret_cast<void*>(&PostLoadHK));
        }
    }
}

void ABuildingContainer::LogWorldContainers(UWorld* World)
{
    // What the world's creation left the containers as. A tiered container that is still there with no mesh index keeps
    // its placeholder mesh on every client (#61); one that rolled no loot was destroyed and waits in the net driver's
    // destroyed startup actors until each client has its level.
    int32_t Containers = 0;
    int32_t WithLootTier = 0;
    int32_t Tiered = 0;
    int32_t TieredWithMesh = 0;
    int32_t TieredDormant = 0;
    ABuildingContainer* FirstWithoutMesh = nullptr;
    for (TObjectIterator<ABuildingContainer> It; It; ++It)
    {
        ABuildingContainer* Container = *It;
        if (Container->GetWorld() != World || Container->HasAnyFlags(static_cast<EObjectFlags>(RF_ClassDefaultObject | RF_ArchetypeObject)))
        {
            continue;
        }
        ++Containers;
        WithLootTier += static_cast<FName>(Container->ContainerLootTierKey).IsNone() ? 0 : 1;
        const TStructArray<FTierMeshSets>& TierMeshSets = Container->AlternateMeshes;
        if (TierMeshSets.Num() <= 0)
        {
            continue;
        }
        ++Tiered;
        TieredDormant += Container->NetDormancy == DORM_Initial ? 1 : 0;
        if (Container->AltMeshIdx != -1)
        {
            ++TieredWithMesh;
        }
        else if (!FirstWithoutMesh)
        {
            FirstWithoutMesh = Container;
        }
    }

    UNetDriver* NetDriver = World ? World->GetNetDriver() : nullptr;
    const int32_t DestroyedActors = NetDriver ? NetDriver->DestroyedStartupOrDormantActors.Num() : -1;
    UE_LOG(LogFortBuilding, Log, TEXT("World containers: %d (%d with a loot tier); tiered %d: %d with a mesh, %d initially dormant, first without a mesh %s in %s; %d destroyed startup or dormant actors to send"),
        Containers, WithLootTier, Tiered, TieredWithMesh, TieredDormant, *GetNameSafe(FirstWithoutMesh), FirstWithoutMesh ? *GetNameSafe(FirstWithoutMesh->GetOutermost()) : TEXT("-"), DestroyedActors);
    // The floor loot this world's creation spawned. A Battle Royale server loads its map again for every match
    // (AFortGameSessionDedicated::Restart after the match, or when the matchmaking service drops it), and each world
    // spawns its own: the counts start again for the next one.
    if (FloorLootContainers > 0)
    {
        UE_LOG(LogFortBuilding, Log, TEXT("Floor loot: %d containers spawned %d pickups (%d loaded weapons)"), FloorLootContainers, FloorLootPickups, FloorLootLoadedWeapons);
    }
    FloorLootContainers = 0;
    FloorLootPickups = 0;
    FloorLootLoadedWeapons = 0;}

void ABuildingContainer::PostLoadHK(ABuildingContainer* This)
{
    This->PostLoad();
}

void ABuildingContainer::PostUpdateHK(ABuildingContainer* This, uint8_t State, FFortBuildingRandomList* RandomList)
{
    This->PostUpdate(EFortBuildingPersistentState{ State }, RandomList);
}

void ABuildingContainer::PostUpdateSuperHK(ABuildingContainer* This, uint8_t State, FFortBuildingRandomList* RandomList)
{
    This->ABuildingSMActor::PostUpdate(EFortBuildingPersistentState{ State }, RandomList);
}

void ABuildingContainer::InitializeTierGroupDataHK(ABuildingContainer* This, uint8_t State, bool bInitializeAgain)
{
    This->InitializeTierGroupData(EFortBuildingPersistentState{ State }, bInitializeAgain);
}

bool ABuildingContainer::SpawnLootHK(ABuildingContainer* This, AFortPawn* Looter)
{
    return This->SpawnLoot(Looter);
}
