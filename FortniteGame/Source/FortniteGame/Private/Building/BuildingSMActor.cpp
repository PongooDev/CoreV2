#include "pch.h"
#include "Building/BuildingSMActor.h"
#include "Building/BuildingContainer.h"
#include "AbilitySystemBlueprintLibrary.h"
#include "Abilities/FortPlayerAttrSet.h"
#include "FortAssetManager.h"
#include "FortGameData.h"
#include "Items/FortItem.h"
#include "Items/FortItemDefinition.h"
#include "Items/FortPickup.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Math/Rotator.h"
#include "Pawns/FortPawn.h"
#include "Player/FortPlayerController.h"
#include "Player/FortPlayerState.h"
#include "ReflectionParams.h"
#include "UObject/UnrealType.h"
#include "Engine/World.h"
#include "Finder.h"
#include "FortGameState.h"
#include "FortKismetLibrary.h"
#include "FortWorldManager.h"
#include "Hooks.h"
#include "Logging/LogMacros.h"
#include "Natives.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"

#include <algorithm>
#include <vector>

DEFINE_LOG_CATEGORY_STATIC(LogFortBuilding, Log, All);

AFortWorldManager* ABuildingSMActor::GetWorldManager() const
{
    UWorld* World = GetWorld();
    AFortGameState* GameState = World ? Cast<AFortGameState>(reinterpret_cast<UObject*>(World->GetGameState())) : nullptr;
    return GameState ? static_cast<AFortWorldManager*>(GameState->WorldManager) : nullptr;
}

int32_t ABuildingSMActor::GetWorldLevel() const
{
    AFortWorldManager* WorldManager = GetWorldManager();
    if (!WorldManager)
    {
        return -1;
    }
    UWorld* World = WorldManager->GetWorld();
    AFortGameState* GameState = World ? Cast<AFortGameState>(reinterpret_cast<UObject*>(World->GetGameState())) : nullptr;
    return GameState ? static_cast<int32_t>(GameState->WorldLevel) : 0;
}

void ABuildingSMActor::UpdateActorRecord(AFortWorldManager* WorldManager, EFortBuildingPersistentState State, bool bForce)
{
    if (WorldManager && (Natives::AFortWorldManager_UpdateActorRecord || Finder::ResolveBuildingLootLive()))
    {
        Natives::AFortWorldManager_UpdateActorRecord(WorldManager, this, State, bForce);
    }
}

void ABuildingSMActor::SetMeshSet(const FMeshSet& MeshSet)
{
    if (Natives::ABuildingSMActor_SetMeshSetIndex < 0 && !Finder::ResolveBuildingLootLive())
    {
        return;
    }
    using FSetMeshSet = void (*)(ABuildingSMActor* This, const FMeshSet& MeshSet);
    reinterpret_cast<FSetMeshSet>((*reinterpret_cast<void***>(this))[Natives::ABuildingSMActor_SetMeshSetIndex])(this, MeshSet);
}

void ABuildingSMActor::SelectMeshSet(int32_t LootTier)
{
    if (Natives::ABuildingSMActor_SelectMeshSet || Finder::ResolveBuildingLootLive())
    {
        Natives::ABuildingSMActor_SelectMeshSet(this, LootTier);
    }
}

void ABuildingSMActor::SelectMeshSet(FName LootTierKey)
{
    const int32_t LootTier = UFortLootTier::FindLootTierFromKey(LootTierKey);
    TStructArray<FTierMeshSets>& TierMeshSets = AlternateMeshes;
    FTierMeshSets* Selected = nullptr;
    for (int32_t Index = 0; Index < TierMeshSets.Num(); ++Index)
    {
        FTierMeshSets& Candidate = TierMeshSets[Index];
        const int32_t Tier = Candidate.Tier;
        if (Tier > LootTier || (Selected && Selected->Tier >= Tier))
        {
            continue;
        }
        if (Candidate.MeshSets.Num() <= 0)
        {
            UE_LOG(LogFortBuilding, Warning, TEXT("No alternative meshes on %s for tier %d"), *GetName(), Tier);
            continue;
        }
        Selected = &Candidate;
    }

    const int32_t MeshIndex = AltMeshIdx;
    if (Selected && MeshIndex != -1 && MeshIndex >= 0 && MeshIndex < Selected->MeshSets.Num())
    {
        SetMeshSet(Selected->MeshSets[MeshIndex]);
    }
    UE_LOG(LogFortBuilding, Verbose, TEXT("ABuildingSMActor::SelectMeshSet(FName LootTierKey) Building: %s, AltMeshIdx: %d"), *GetName(), static_cast<int32_t>(AltMeshIdx));
}

void ABuildingSMActor::PostUpdate(EFortBuildingPersistentState State, FFortBuildingRandomList* RandomList)
{
#if WITH_SERVER_CODE
    const FName CurrentKey = DestructionLootTierKey;
    if (!CurrentKey.IsNone())
    {
        SelectMeshSet(CurrentKey);
    }
    else if (!static_cast<FName>(DestructionLootTierGroup).IsNone())
    {
        int32_t LootTier = -1;
        const FChosenQuotaInfo& ChosenQuota = DestructionLootTierChosenQuotaInfo;
        if (ChosenQuota.IsSet())
        {
            DestructionLootTierKey = ChosenQuota.LootTierKey;
            LootTier = ChosenQuota.LootTier;
        }
        else
        {
            FName PickedKey = DestructionLootTierKey;
            UFortKismetLibrary::PickLootTierKeyAndLootTierFromTierGroup(PickedKey, LootTier, DestructionLootTierGroup, GetWorldLevel(), -1, StaticGameplayTags);
            DestructionLootTierKey = PickedKey;
        }
        SelectMeshSet(LootTier);

        EFortBuildingPersistentState NewState = EFortBuildingPersistentState::GetDefault();
        if (!UFortLootTier::ShouldDropLoot(DestructionLootTierKey, L"ABuildingSMActor::AttemptSpawnResources"))
        {
            DestructionLootTierKey = FName();
            NewState = EFortBuildingPersistentState::GetSearched();
        }
        if (AltMeshIdx != -1 && bPersistToWorld)
        {
            UpdateActorRecord(GetWorldManager(), NewState, true);
        }
    }
    UE_LOG(LogFortBuilding, Verbose, TEXT("ABuildingSMActor::PostUpdate() Building: %s, AltMeshIdx: %d"), *GetName(), static_cast<int32_t>(AltMeshIdx));
#endif
}

float& ABuildingSMActor::ResourceRemainder()
{
    // The fraction of a resource left over from the last hit: the native float after MaxResourcesToSpawn.
    return *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(&GetMember_MaxResourcesToSpawn()) + sizeof(int32_t));
}

bool ABuildingSMActor::ResolveHarvest()
{
    // The mask is only set once the offset resolves, and the arguments are evaluated right to left.
    const ptrdiff_t AllowResourceDropOffset = Member_bAllowResourceDrop.GetOffsetChecked();
    return Finder::ResolveHarvestLive(AllowResourceDropOffset, Member_bAllowResourceDrop.GetFieldMask());
}

float ABuildingSMActor::DetermineMaxResourcesToSpawn(bool bRandomize)
{
    if (Natives::ABuildingSMActor_DetermineMaxResourcesToSpawnIndex < 0 && !ResolveHarvest())
    {
        return 0.0f;
    }
    using FDetermineMaxResourcesToSpawn = float (*)(ABuildingSMActor* This, bool bRandomize);
    return reinterpret_cast<FDetermineMaxResourcesToSpawn>((*reinterpret_cast<void***>(this))[Natives::ABuildingSMActor_DetermineMaxResourcesToSpawnIndex])(this, bRandomize);
}

bool ABuildingSMActor::ResolveLootQuota()
{
    return Natives::ABuildingSMActor_GetMaximumQuotaLevelBoundIndex >= 0 || Finder::ResolveLootQuotaLive(Member_MaximumQuotaLevelBound.GetOffsetChecked());
}

void* ABuildingSMActor::GetQuotaFunction(int32_t SlotFromBound)
{
    if (!ResolveLootQuota())
    {
        return nullptr;
    }
    return (*reinterpret_cast<void***>(this))[Natives::ABuildingSMActor_GetMaximumQuotaLevelBoundIndex + SlotFromBound];
}

const FName& ABuildingSMActor::GetQuotaLootTierGroup()
{
    using FGetQuotaLootTierGroup = const FName* (*)(ABuildingSMActor* This);
    void* Function = GetQuotaFunction(-2);
    return Function ? *reinterpret_cast<FGetQuotaLootTierGroup>(Function)(this) : DestructionLootTierGroup;
}

bool ABuildingSMActor::CanSpawnAtLowerQuotaLevels()
{
    using FCanSpawnAtLowerQuotaLevels = bool (*)(ABuildingSMActor* This);
    void* Function = GetQuotaFunction(-1);
    return Function ? reinterpret_cast<FCanSpawnAtLowerQuotaLevels>(Function)(this) : bCanSpawnAtLowerQuotaLevels;
}

ELootQuotaLevel ABuildingSMActor::GetMaximumQuotaLevelBound()
{
    using FGetMaximumQuotaLevelBound = uint8_t* (*)(ABuildingSMActor* This, uint8_t* OutLevel);
    void* Function = GetQuotaFunction(0);
    uint8_t Level = MaximumQuotaLevelBound;
    if (Function)
    {
        reinterpret_cast<FGetMaximumQuotaLevelBound>(Function)(this, &Level);
    }
    return ELootQuotaLevel{ Level };
}

void ABuildingSMActor::SetChosenQuotaInfo(const FName& LootTierKey, int32_t LootTier)
{
    using FSetChosenQuotaInfo = void (*)(ABuildingSMActor* This, const FName* LootTierKey, int32_t LootTier);
    if (void* Function = GetQuotaFunction(1))
    {
        reinterpret_cast<FSetChosenQuotaInfo>(Function)(this, &LootTierKey, LootTier);
    }
}

void ABuildingSMActor::OnQuotaNotChosen()
{
    using FOnQuotaNotChosen = void (*)(ABuildingSMActor* This);
    if (void* Function = GetQuotaFunction(3))
    {
        reinterpret_cast<FOnQuotaNotChosen>(Function)(this);
    }
}

void ABuildingSMActor::AttemptSpawnResources(AFortPawn* InstigatorPawn, float Damage, bool bJustHitWeakspot)
{
#if WITH_SERVER_CODE
    if (!InstigatorPawn || Damage <= 0.0f || Role != ROLE_Authority)
    {
        return;
    }
    AFortPlayerController* PlayerController = Cast<AFortPlayerController>(reinterpret_cast<UObject*>(static_cast<AController*>(InstigatorPawn->Controller)));
    if (!PlayerController)
    {
        return;
    }

    UFortGameData* GameData = UFortAssetManager::Get().GetGameData();
    const uint8_t Resource = ResourceType;
    UFortWorldItemDefinition* ResourceItem = nullptr;
    if (GameData)
    {
        if (Resource == EFortResourceType::GetWood())
        {
            ResourceItem = GameData->WoodItemDefinition;
        }
        else if (Resource == EFortResourceType::GetStone())
        {
            ResourceItem = GameData->StoneItemDefinition;
        }
        else if (Resource == EFortResourceType::GetMetal())
        {
            ResourceItem = GameData->MetalItemDefinition;
        }
        else if (Resource == EFortResourceType::GetPermanite())
        {
            ResourceItem = GameData->PermaniteItemDefinition;
        }
    }

    UAbilitySystemComponent* AbilitySystemComponent = UAbilitySystemBlueprintLibrary::GetAbilitySystemComponent(InstigatorPawn);
    int32_t ResourceCount = 0;
    float ResourcesPerHealth = 0.0f;
    if (ResourceItem)
    {
        if (MaxResourcesToSpawn < 0)
        {
            MaxResourcesToSpawn = static_cast<int32_t>(DetermineMaxResourcesToSpawn(true));
        }
        FGameplayTagContainer ResourceTags;
        ResourceTags.AppendTags(ResourceItem->GameplayTags);
        ResourcesPerHealth = static_cast<float>(MaxResourcesToSpawn) / GetMaxHealth();
        float Amount = ResourcesPerHealth * Damage;
        if (AbilitySystemComponent)
        {
            static FGameplayAttribute ResourceHarvesting;
            if (!ResourceHarvesting.IsValid() && UFortPlayerAttrSet::StaticClass())
            {
                ResourceHarvesting = FGameplayAttribute(UFortPlayerAttrSet::StaticClass()->FindPropertyByName(FName(L"ResourceHarvesting")));
            }
            bool bSuccess = false;
            const float Harvested = UAbilitySystemBlueprintLibrary::EvaluateAttributeValueWithTagsAndBase(AbilitySystemComponent, ResourceHarvesting, FGameplayTagContainer(), ResourceTags, Amount, bSuccess);
            if (bSuccess)
            {
                Amount = Harvested;
            }
        }
        Amount += ResourceRemainder();
        ResourceCount = static_cast<int32_t>(Amount);
        ResourceRemainder() = Amount - static_cast<float>(ResourceCount);
    }

    UWorld* World = GetWorld();
    USceneComponent* Root = GetRootComponent();
    const FRotator SpawnRotation(0.0f, 0.0f, 0.0f);
    const auto SpawnLocation = [Root]()
    {
        FVector Origin;
        FVector BoxExtent;
        float SphereRadius = 0.0f;
        UKismetSystemLibrary::GetComponentBounds(Root, Origin, BoxExtent, SphereRadius);
        return Origin + UKismetMathLibrary::RandomUnitVector() * 200.0f;
    };
    UScriptStruct* EntryStruct = FFortItemEntry::StaticStruct();

    bool bDestroyed = false;
    if (GetHealth() <= 0.0f)
    {
        bDestroyed = true;
        ResourceCount = FMath::Max(ResourceCount, ResourceItem && ResourcesPerHealth != 0.0f ? 1 : 0);

        const FName LootTierKey = DestructionLootTierKey;
        if (!LootTierKey.IsNone())
        {
            TStructArray<FFortItemEntry> LootDrops{};
            UFortLootPackage::PickLootDrops(LootDrops, GetWorldManager() ? GetWorldLevel() : -1, LootTierKey, AbilitySystemComponent, StaticGameplayTags, true, false);
            for (int32_t Index = 0; Index < LootDrops.Num(); ++Index)
            {
                UFortItemDefinition* ItemDefinition = LootDrops[Index].ItemDefinition;
                if (!Cast<UFortWorldItemDefinition>(ItemDefinition))
                {
                    UE_LOG(LogFortBuilding, Error, TEXT("Loot tier %s dropped entry with no item data!"), *LootTierKey.ToString());
                    continue;
                }

                const FVector Location = SpawnLocation();
                std::vector<uint8_t> EntryStorage(EntryStruct->GetStructureSize(), 0);
                FFortItemEntry* Entry = reinterpret_cast<FFortItemEntry*>(EntryStorage.data());
                FFortItemEntry::Construct(Entry, ItemDefinition, LootDrops[Index].Count, 0);
                Entry->SetStateValue(EFortItemEntryState::GetDoNotShowSpawnParticles(), 1);
                if (AFortPickup* Pickup = AFortPickup::CreatePickup(World, *Entry, &Location, &SpawnRotation))
                {
                    const FVector Direction = UKismetMathLibrary::RandomUnitVectorInCone(FVector(0.0f, 0.0f, 1.0f), 0.2617994f);
                    // Staggered by 0.3 s per drop from the first, computed in double as the native does.
                    const float FlyTime = static_cast<float>(static_cast<double>(AFortPickup::GetRandomFlyTime()) + static_cast<double>(Index + 1) * 0.3);
                    Pickup->SetPickupTarget(InstigatorPawn, FlyTime, Direction, true);
                }
                EntryStruct->DestroyStruct(Entry);
            }
            LootDrops.Empty();
        }
    }

    if (ResourceCount > 0)
    {
        std::vector<uint8_t> EntryStorage(EntryStruct->GetStructureSize(), 0);
        FFortItemEntry* Entry = reinterpret_cast<FFortItemEntry*>(EntryStorage.data());
        FFortItemEntry::Construct(Entry, ResourceItem, ResourceCount, 0);
        const FVector Location = SpawnLocation();
        Entry->SetStateValue(EFortItemEntryState::GetDoNotShowSpawnParticles(), 1);
        if (AFortPickup* Pickup = AFortPickup::CreatePickup(World, *Entry, &Location, &SpawnRotation))
        {
            if (OwnerPersistentID == -1)
            {
                AFortPlayerStateZone* PlayerState = Cast<AFortPlayerStateZone>(reinterpret_cast<UObject*>(static_cast<APlayerState*>(PlayerController->PlayerState)));
                const uint8_t StatType = ResourceItem->AccumulatingStatType;
                if (PlayerState && StatType != EFortReplicatedStat::GetNone())
                {
                    PlayerState->ModifyStat(EFortReplicatedStat{ StatType }, ResourceCount, ResourceCount, GetCentroid());
                }
            }
            const FVector Direction = UKismetMathLibrary::RandomUnitVector();
            Pickup->SetPickupTarget(InstigatorPawn, AFortPickup::GetRandomFlyTime(), Direction, true);
            PlayerController->ClientReportDamagedResourceBuilding(this, Resource, ResourceCount, bDestroyed, bJustHitWeakspot);
        }
        EntryStruct->DestroyStruct(Entry);
    }
#endif
}

void ABuildingSMActor::HookVTables()
{
    // 1.2: the resource spawn every building inherits is the shared empty function.
    if (Natives::ABuildingSMActor_AttemptSpawnResourcesIndex >= 0 || ResolveHarvest())
    {
        const int32_t HarvestIndex = Natives::ABuildingSMActor_AttemptSpawnResourcesIndex;
        for (TObjectIterator<UClass> It; It; ++It)
        {
            UClass* Class = *It;
            UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
            void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
            if (VTable && Finder::IsStubFunction(reinterpret_cast<uintptr_t>(VTable[HarvestIndex])))
            {
                Hooks::SwapVTable(L"ABuildingSMActor::AttemptSpawnResources", VTable, HarvestIndex, reinterpret_cast<void*>(&AttemptSpawnResourcesHK));
            }
        }
    }

    if (Natives::ABuildingSMActor_PostUpdateIndex < 0 && !Finder::ResolveBuildingLootLive())
    {
        return;
    }
    if (!Natives::ABuildingSMActor_bPostUpdateStripped)
    {
        return;
    }

    // 1.2: every building's PostUpdate is the logging stub, or an override that calls it (foundations).
    const int32_t Index = Natives::ABuildingSMActor_PostUpdateIndex;
    const uintptr_t Stripped = Natives::ABuildingSMActor_PostUpdate;
    std::vector<uintptr_t> Overrides;
    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Class = *It;
        if (!Class->IsChildOf(StaticClass()) || Class->IsChildOf(ABuildingContainer::StaticClass()))
        {
            continue;
        }
        UObject* ClassDefault = Class->GetDefaultObject(false);
        void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
        if (!VTable)
        {
            continue;
        }
        const uintptr_t Entry = reinterpret_cast<uintptr_t>(VTable[Index]);
        if (Entry == Stripped)
        {
            Hooks::SwapVTable(L"ABuildingSMActor::PostUpdate", VTable, Index, reinterpret_cast<void*>(&PostUpdateHK));
        }
        else if (Entry != reinterpret_cast<uintptr_t>(&PostUpdateHK) && std::find(Overrides.begin(), Overrides.end(), Entry) == Overrides.end())
        {
            Overrides.push_back(Entry);
        }
    }
    for (const uintptr_t Override : Overrides)
    {
        for (const auto& [Site, Target] : Finder::DirectCalls(Override, Finder::FunctionEnd(Override)))
        {
            if (Target == Stripped)
            {
                Hooks::RedirectCall(L"ABuildingSMActor::PostUpdate (Super)", reinterpret_cast<uint8_t*>(Site), reinterpret_cast<void*>(&PostUpdateHK));
            }
        }
    }
}

void ABuildingSMActor::AttemptSpawnResourcesHK(ABuildingSMActor* This, AFortPawn* InstigatorPawn, float Damage, bool bJustHitWeakspot)
{
    This->AttemptSpawnResources(InstigatorPawn, Damage, bJustHitWeakspot);
}

void ABuildingSMActor::PostUpdateHK(ABuildingSMActor* This, uint8_t State, FFortBuildingRandomList* RandomList)
{
    This->PostUpdate(EFortBuildingPersistentState{ State }, RandomList);
}
