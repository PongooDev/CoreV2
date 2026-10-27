#include "pch.h"
#include "Building/BuildingContainer.h"
#include "AbilitySystemBlueprintLibrary.h"
#include "Athena/FortGameModeAthena.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/NetDriver.h"
#include "Engine/World.h"
#include "Finder.h"
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

    // How far a trace from Start gets across the floor along Direction, up to Distance, past Container. A trace that starts
    // inside something hits it where it starts, which says nothing about the way ahead.
    float TraceReach(ABuildingContainer* Container, const FVector& Start, const FVector& Direction, float Distance)
    {
        FVector Hit;
        if (!UKismetSystemLibrary::LineTraceSingle(Container, Start, Start + Direction * Distance, 0, false, true, Hit))
        {
            return Distance;
        }
        const float Reach = std::sqrt(FMath::Square(Hit.X - Start.X) + FMath::Square(Hit.Y - Start.Y));
        return Reach < 1.0f ? Distance : Reach;
    }

    int32_t FloorLootContainers = 0;
    int32_t FloorLootPickups = 0;
    int32_t FloorLootLoadedWeapons = 0;
}

bool ABuildingContainer::IsAlreadySearched(AFortPawn* Looter)
{
    return ContainerSlot<bool (*)(ABuildingContainer*, AFortPawn*)>(this, -3)(this, Looter);
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
    ContainerSlot<void (*)(ABuildingContainer*)>(this, 3)(this);
}

void ABuildingContainer::SetSearched(AFortPawn* Searcher)
{
    ContainerSlot<void (*)(ABuildingContainer*, AFortPawn*)>(this, 4)(this, Searcher);
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

void ABuildingContainer::GetFloorLootLocations(int32_t Index, int32_t Count, FVector& OutSpawnLocation, FVector& OutFinalLocation)
{
    // Floor loot (Tiered_Athena_FloorLoot_01) is out before anyone lands, so it is only dropped into place (#72): each item
    // gets its own spot around the container's pivot (its floor) and drops from LootSpawnLocation's height a little short
    // of it, never straight above it: AFortPickup::TossPickup (1.7.2 0x6622F0) sends a toss with no distance in a random
    // direction 512 away.
    constexpr float FloorLootRadius = 40.0f;
    constexpr float DropShortfall = 20.0f;
    const FTransform ContainerTransform = GetTransform();
    const FVector Base = ContainerTransform.GetLocation();
    const FVector Forward = GetAthenaTossForward();
    const FVector Right(-Forward.Y, Forward.X, 0.0f);
    const float Height = FMath::Max(ContainerTransform.TransformPosition(LootSpawnLocation).Z - Base.Z, 30.0f);
    const float Angle = Count > 1 ? 2.0f * 3.1415926535897932f * static_cast<float>(Index) / static_cast<float>(Count) : 0.0f;
    const FVector Direction = Forward * std::cos(Angle) + Right * std::sin(Angle);
    OutFinalLocation = Base + Direction * (Count > 1 ? FloorLootRadius : DropShortfall);
    OutSpawnLocation = OutFinalLocation - Direction * DropShortfall + FVector(0.0f, 0.0f, Height);
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
    // LootSpawnLocation (a tiered chest's mesh sets: 100 over its pivot) is inside the container's top, and a pickup's
    // collision cylinder (radius 30, half height 80: AFortPickup's constructor, 1.7.2 0x65BA90) sweeps against it, so loot
    // launched from there with little forward speed came down on the lid. It leaves at that height from just past the
    // front of the container's collision instead.
    constexpr float PickupRadius = 30.0f;
    const FVector Location = GetTransform().TransformPosition(LootSpawnLocation);
    const FVector Forward = GetAthenaTossForward();
    return Location + Forward * std::max(GetAthenaLootFront() + PickupRadius - (Location | Forward), 0.0f);
}

std::vector<FVector> ABuildingContainer::GetAthenaLootSpots(int32_t Count)
{
    // Battle Royale's chests and ammo boxes pop their loot out onto the floor just in front of them (#90, #95). Launched
    // at the container's own toss settings (LootTossDirection_Athena pitch 65, LootTossSpeed_Athena 550), the items went
    // where the bounces took them: on a 1.7.2 server, 0.9 to 2.2m past the front of a chest in the open, 12m down off a
    // ledge, or in a heap where they started with a wall in front. Each item gets its own spot instead, for TossOntoFloor
    // to land it on: two staggered rows 50 and 90 past the front (pickups are 60 across), side by side across the front,
    // each moved a little at random. A wall pulls the rows in towards the container (never back over it) and pushes the
    // row along the front, or closes it up, and each spot is traced down to the floor below it, from a little over the
    // container's floor. On the same server, items in the open came to rest within 10 of their spots, 50 to 90 past the
    // front and about 60 apart; on a slope they roll on a little, up to 1.5m out.
    constexpr float PickupRadius = 30.0f;
    constexpr float NearRow = 50.0f;
    constexpr float FarRow = 90.0f;
    constexpr float PickupSpacing = 70.0f;
    constexpr float Jitter = 10.0f;
    constexpr float TraceHeight = 50.0f;
    constexpr float TraceDepth = 100000.0f;
    const FVector Base = GetTransform().GetLocation();
    const FVector Forward = GetAthenaTossForward();
    const FVector Right(-Forward.Y, Forward.X, 0.0f);
    const FVector Center(Base.X, Base.Y, Base.Z + TraceHeight);
    const float Front = GetAthenaLootFront() - (Base | Forward);

    const float Ahead = TraceReach(this, Center, Forward, Front + FarRow + PickupRadius) - Front - PickupRadius;
    const float Near = FMath::Clamp(Ahead, PickupRadius, NearRow);
    const float Far = FMath::Clamp(Ahead, Near, FarRow);
    const float Stagger = Count > 1 ? Far - Near : 0.0f;
    const float Spacing = std::sqrt(std::max(PickupSpacing * PickupSpacing - Stagger * Stagger, PickupSpacing * PickupSpacing * 0.25f));
    const float Width = static_cast<float>(std::max(Count - 1, 0)) * Spacing;
    const FVector Row = Center + Forward * (Front + Near);
    const float LeftRoom = std::max(TraceReach(this, Row, Right * -1.0f, Width + PickupRadius) - PickupRadius, 0.0f);
    const float RightRoom = std::max(TraceReach(this, Row, Right, Width + PickupRadius) - PickupRadius, 0.0f);
    float First = -Width * 0.5f;
    float Step = Spacing;
    if (LeftRoom + RightRoom >= Width)
    {
        First = FMath::Clamp(First, -LeftRoom, RightRoom - Width);
    }
    else
    {
        First = -LeftRoom;
        Step = Count > 1 ? (LeftRoom + RightRoom) / static_cast<float>(Count - 1) : 0.0f;
    }

    std::vector<FVector> Spots;
    for (int32_t Index = 0; Index < Count; ++Index)
    {
        const float Side = First + Step * static_cast<float>(Index) + (FMath::FRand() - 0.5f) * Jitter;
        const float RowOut = (Count > 1 ? (Index % 2 ? Far : Near) : (Near + Far) * 0.5f) - Near;
        const FVector RowSpot = Row + Right * Side;
        const float Room = std::max(TraceReach(this, RowSpot, Forward, RowOut + Jitter + PickupRadius) - PickupRadius, 0.0f);
        const FVector Spot = RowSpot + Forward * FMath::Clamp(RowOut + (FMath::FRand() - 0.5f) * Jitter, 0.0f, Room);
        FVector Floor = Spot;
        UKismetSystemLibrary::LineTraceSingle(this, Spot, FVector(Spot.X, Spot.Y, Spot.Z - TraceDepth), 0, false, true, Floor);
        Spots.push_back(Floor);
    }
    return Spots;
}

void ABuildingContainer::SpawnFloorLoot()
{
    // Battle Royale's floor loot (Tiered_Athena_FloorLoot_01: Loot_AthenaFloorLoot, bStartAlreadySearched_Athena,
    // bDestroyContainerOnSearch) is never searched: its roll lies on the ground from the start, without the container.
    const int32_t WorldLevel = GetWorldManager() ? GetWorldLevel() : -1;
    FName LootTierKey;
    int32_t LootTier = -1;
    UFortKismetLibrary::PickLootTierKeyAndLootTierFromTierGroup(LootTierKey, LootTier, GetSearchLootTierGroup(), WorldLevel, -1, StaticGameplayTags);
    ++FloorLootContainers;
    if (!UFortLootTier::ShouldDropLoot(LootTierKey, L"ABuildingContainer::PostUpdate"))
    {
        return;
    }

    TStructArray<FFortItemEntry> LootDrops{};
    UFortLootPackage::PickLootDrops(LootDrops, WorldLevel, LootTierKey, nullptr, StaticGameplayTags, true, false);
    AFortPickup::CombineItemEntries(LootDrops, nullptr);
    UWorld* World = GetWorld();
    const FRotator SpawnRotation(0.0f, 0.0f, 0.0f);
    for (int32_t Index = 0; Index < LootDrops.Num(); ++Index)
    {
        FFortItemEntry& Entry = LootDrops[Index];
        UFortWorldItemDefinition* WorldItemDefinition = Cast<UFortWorldItemDefinition>(static_cast<UFortItemDefinition*>(Entry.ItemDefinition));
        if (!WorldItemDefinition)
        {
            continue;
        }
        ApplyLootedDurability(Entry, WorldItemDefinition, LootedWeaponsDurabilityModifier);
        FloorLootLoadedWeapons += Entry.LoadFullClip() ? 1 : 0;
        FVector SpawnLocation;
        FVector FinalLocation;
        GetFloorLootLocations(Index, LootDrops.Num(), SpawnLocation, FinalLocation);
        if (AFortPickup* Pickup = AFortPickup::CreatePickup(World, Entry, &SpawnLocation, &SpawnRotation))
        {
            Pickup->TossPickup(FinalLocation, nullptr, 0);
            ++FloorLootPickups;
        }
    }
    LootDrops.Empty();
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

void ABuildingContainer::PostUpdate(EFortBuildingPersistentState State, FFortBuildingRandomList* RandomList)
{
#if WITH_SERVER_CODE
    ABuildingSMActor::PostUpdate(State, RandomList);

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

        const float Roll = RandomList ? RandomList->Next() : 0.0f;
        if (Roll * 1.0f + 0.00000001f > ShowChance)
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
    if (Role < ROLE_Authority || !Looter)
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
            UFortLootPackage::PickLootDrops(LootDrops, WorldManager ? GetWorldLevel() : -1, ContainerLootTierKey, UAbilitySystemBlueprintLibrary::GetAbilitySystemComponent(Looter), StaticGameplayTags, true, false);

            AFortPlayerController* LooterController = Cast<AFortPlayerController>(reinterpret_cast<UObject*>(static_cast<AController*>(Looter->Controller)));
            ModifyLootDrops(LootDrops, LooterController);
            AFortPickup::CombineItemEntries(LootDrops, LooterController ? static_cast<AFortInventory*>(LooterController->WorldInventory) : nullptr);
            const std::vector<FVector> LootSpots = bAthena ? GetAthenaLootSpots(LootDrops.Num()) : std::vector<FVector>();
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
                        Pickup->TossFromContainer(LootSpots[Index]);
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
        const uintptr_t SpawnLoot = reinterpret_cast<uintptr_t>(VTable[SpawnLootIndex]);
        if (Finder::IsStubFunction(SpawnLoot))
        {
            Hooks::SwapVTable(L"ABuildingContainer::SpawnLoot", VTable, SpawnLootIndex, reinterpret_cast<void*>(&SpawnLootHK));
        }
        // AKeepItemCoreContainer's override tail-jumps to the stubbed parent.
        else if (const uintptr_t SpawnLootEnd = SpawnLoot >= Finder::TextBegin() && SpawnLoot < Finder::TextEnd() ? Finder::FunctionEnd(SpawnLoot) : 0; SpawnLootEnd > SpawnLoot + 5 && *reinterpret_cast<const uint8_t*>(SpawnLootEnd - 5) == 0xE9 && Finder::IsStubFunction(Finder::RelativeTarget(SpawnLootEnd - 5, 1, 5)))
        {
            Hooks::RedirectCall(L"ABuildingContainer::SpawnLoot (Super)", reinterpret_cast<uint8_t*>(SpawnLootEnd - 5), reinterpret_cast<void*>(&SpawnLootHK));
        }

        const uintptr_t PostUpdate = reinterpret_cast<uintptr_t>(VTable[PostUpdateIndex]);
        const bool bJumpsToStripped = StrippedPostUpdate && *reinterpret_cast<const uint8_t*>(PostUpdate) == 0xE9 && Finder::RelativeTarget(PostUpdate, 1, 5) == StrippedPostUpdate;
        if (StrippedPostUpdate && (PostUpdate == StrippedPostUpdate || bJumpsToStripped))
        {
            Hooks::SwapVTable(L"ABuildingContainer::PostUpdate", VTable, PostUpdateIndex, reinterpret_cast<void*>(&PostUpdateHK));
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
    if (FloorLootContainers > 0)
    {
        UE_LOG(LogFortBuilding, Log, TEXT("Floor loot: %d containers spawned %d pickups (%d loaded weapons)"), FloorLootContainers, FloorLootPickups, FloorLootLoadedWeapons);
    }
}

void ABuildingContainer::PostLoadHK(ABuildingContainer* This)
{
    This->PostLoad();
}

void ABuildingContainer::PostUpdateHK(ABuildingContainer* This, uint8_t State, FFortBuildingRandomList* RandomList)
{
    This->PostUpdate(EFortBuildingPersistentState{ State }, RandomList);
}

bool ABuildingContainer::SpawnLootHK(ABuildingContainer* This, AFortPawn* Looter)
{
    return This->SpawnLoot(Looter);
}
