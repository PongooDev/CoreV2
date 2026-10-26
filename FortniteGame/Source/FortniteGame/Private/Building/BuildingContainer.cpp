#include "pch.h"
#include "Building/BuildingContainer.h"
#include "AbilitySystemBlueprintLibrary.h"
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
            const FChosenQuotaInfo& ChosenQuota = SearchLootTierChosenQuotaInfo;
            if (ChosenQuota.IsSet())
            {
                ContainerLootTierKey = ChosenQuota.LootTierKey;
                LootTier = ChosenQuota.LootTier;
            }
            else
            {
                FName PickedKey = ContainerLootTierKey;
                UFortKismetLibrary::PickLootTierKeyAndLootTierFromTierGroup(PickedKey, LootTier, SearchLootTierGroup, WorldManager ? GetWorldLevel() : -1, -1, StaticGameplayTags);
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
            UFortKismetLibrary::PickLootTierKeyAndLootTierFromTierGroup(PickedKey, LootTier, SearchLootTierGroup, GetWorldLevel(), -1, StaticGameplayTags);
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
    if (!IsAlreadySearched(Looter))
    {
        AFortWorldManager* WorldManager = GetWorldManager();
        const FVector SpawnLocation = ContainerTransform.TransformPosition(LootSpawnLocation);
        const FRotator SpawnRotation(0.0f, 0.0f, 0.0f);
        UWorld* World = GetWorld();
        UScriptStruct* EntryStruct = FFortItemEntry::StaticStruct();

        if (UFortWorldItemDefinition* TestingData = LootTestingData)
        {
            std::vector<uint8_t> EntryStorage(EntryStruct->GetStructureSize(), 0);
            FFortItemEntry* Entry = reinterpret_cast<FFortItemEntry*>(EntryStorage.data());
            FFortItemEntry::Construct(Entry, TestingData, 1, 0);
            ApplyLootedDurability(*Entry, TestingData, LootedWeaponsDurabilityModifier);
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
                if (AFortPickup* Pickup = AFortPickup::CreatePickup(World, Entry, &SpawnLocation, &SpawnRotation))
                {
                    const FVector Direction = UKismetMathLibrary::RandomUnitVectorInCone(FVector(0.0f, 0.0f, 1.0f), 0.2617994f);
                    const float FlyTime = AFortPickup::GetRandomFlyTime() + static_cast<float>(Index) * 0.3f;
                    Pickup->SetPickupTarget(Looter, FlyTime, Direction, true);
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
