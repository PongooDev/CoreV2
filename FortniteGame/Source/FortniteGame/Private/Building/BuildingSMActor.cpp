#include "pch.h"
#include "Building/BuildingSMActor.h"
#include "Building/BuildingContainer.h"
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

void ABuildingSMActor::HookVTables()
{
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

void ABuildingSMActor::PostUpdateHK(ABuildingSMActor* This, uint8_t State, FFortBuildingRandomList* RandomList)
{
    This->PostUpdate(EFortBuildingPersistentState{ State }, RandomList);
}
