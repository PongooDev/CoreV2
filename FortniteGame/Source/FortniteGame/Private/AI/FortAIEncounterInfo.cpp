#include "pch.h"
#include "AI/FortAIEncounterInfo.h"
#include "Finder.h"
#include "FortKismetLibrary.h"
#include "Hooks.h"
#include "Items/FortItem.h"
#include "Items/FortLootPackage.h"
#include "Natives.h"
#include "ReflectionParams.h"

namespace
{
    // What the stripped upgrade picked, captured from its own calls while it runs.
    struct FSpawnGroupUpgradeCapture
    {
        const FFortAISpawnGroupUpgrade* Upgrade = nullptr;
        float DifficultyLevel = 0.0f;
        FGameplayTagContainer EnemyTags;
    };

    FSpawnGroupUpgradeCapture* Capture = nullptr;
}

bool UFortAIEncounterInfo::SpawnAIGroup(const UFortAISpawnGroup* SpawnGroupToSpawn, const FVector& SpawnLocation, const FRotator& SpawnRotation, bool bAllowAssigningToExternalSpawners)
{
    // The native (1.7.2 0x2D74B0) adds the group to ActiveSpawnGroups and queues one pending spawn per enemy with the
    // director, SecondsBetweenSpawns apart.
    UFunction* Function = FindStaticFunction_SpawnAIGroup();
    if (!Function)
    {
        return false;
    }
    Reflection::FParams Params(Function, true);
    Params.Set(L"SpawnGroupToSpawn", SpawnGroupToSpawn);
    Params.Set(L"SpawnLocation", SpawnLocation);
    Params.Set(L"SpawnRotation", SpawnRotation);
    Params.Set(L"SpawnSource", static_cast<UObject*>(nullptr));
    Params.Set(L"bAllowAssigningToExternalSpawners", bAllowAssigningToExternalSpawners);
    Params.Set(L"SecondsBetweenSpawns", 0.25f);
    return Params.Call(this);
}

void UFortAIEncounterInfo::RollSpawnGroupUpgradeModifiers(const FGuid& GroupGuid, const FFortAISpawnGroupUpgrade& Upgrade, float DifficultyLevel, const FGameplayTagContainer& EnemyTags)
{
#if WITH_SERVER_CODE
    // The upgrade applies to the last instance with the guid.
    FSpawnGroupInstanceInfo* Instance = nullptr;
    TStructArray<FSpawnGroupInstanceInfo>& Groups = ActiveSpawnGroups;
    for (int32_t Index = 0; Index < Groups.Num(); ++Index)
    {
        if (Groups[Index].GroupGuid == GroupGuid)
        {
            Instance = &Groups[Index];
        }
    }
    UFortAISpawnGroup* SpawnGroup = Instance ? Instance->SpawnGroup : nullptr;
    if (!SpawnGroup)
    {
        return;
    }

    FGameplayTagContainer ContextTags = SpawnGroup->SpawnGroupGameplayTags;
    ContextTags.AppendTags(EnemyTags);

    const int32_t WorldLevel = static_cast<int32_t>(DifficultyLevel);
    FName LootTierKey;
    int32_t LootTier = -1;
    if (!UFortKismetLibrary::PickLootTierKeyAndLootTierFromTierGroup(LootTierKey, LootTier, Upgrade.AdditionalModifiersLootTierGroup, WorldLevel, -1, ContextTags))
    {
        return;
    }

    TStructArray<FFortItemEntry> LootDrops{};
    UFortLootPackage::PickLootDrops(LootDrops, WorldLevel, LootTierKey, nullptr, ContextTags, true, false);
    for (int32_t Index = 0; Index < LootDrops.Num(); ++Index)
    {
        UFortItemDefinition* ItemDefinition = LootDrops[Index].ItemDefinition;
        if (ItemDefinition && ItemDefinition->IsA(UFortGameplayModifierItemDefinition::StaticClass()))
        {
            Instance->ModifiersForAllPawns.Add(ItemDefinition);
        }
    }
    LootDrops.Empty();
#endif
}

void UFortAIEncounterInfo::HookCallSites()
{
#if WITH_SERVER_CODE
    Finder::FSpawnGroupUpgradeSites Sites;
    if (!Finder::ResolveSpawnGroupUpgradeLive(Member_AvailableUpgrades.GetOffsetChecked(), Member_ActiveSpawnGroups.GetOffsetChecked(), FSpawnGroupInstanceInfo::Member_TotalGroupCost.GetOffsetChecked(), FSpawnGroupInstanceInfo::Member_PawnUpgrades.GetOffsetChecked(), FSpawnGroupInstanceInfo::Member_ModifiersForAllPawns.GetOffsetChecked(), Sites))
    {
        return;
    }

    // 1.2 cut the roll of the chosen upgrade's modifiers: capture the upgrade, difficulty and enemy tags it still uses, then roll.
    if (!Sites.bRollsModifiers)
    {
        Hooks::RedirectCall(L"UFortAISpawnGroup::GetSpawnGroupCost (upgrade)", Sites.GroupCostCall, reinterpret_cast<void*>(&GetSpawnGroupCostCall));
        Hooks::RedirectCall(L"FFortAISpawnGroupUpgrade::PickPawnUpgrade", Sites.PawnUpgradeCall, reinterpret_cast<void*>(&PickPawnUpgradeCall));
        Hooks::RedirectCall(L"UFortAIEncounterInfo::TryUpgradeSpawnGroup", Sites.TryUpgradeCall, reinterpret_cast<void*>(&TryUpgradeSpawnGroupCall));
    }
#endif
}

void UFortAIEncounterInfo::TryUpgradeSpawnGroupCall(UFortAIEncounterInfo* This, const FGuid& GroupGuid, bool bForEnemySpawner)
{
    FSpawnGroupUpgradeCapture Captured;
    Capture = &Captured;
    Natives::UFortAIEncounterInfo_TryUpgradeSpawnGroup(This, &GroupGuid, bForEnemySpawner);
    Capture = nullptr;

    if (Captured.Upgrade)
    {
        This->RollSpawnGroupUpgradeModifiers(GroupGuid, *Captured.Upgrade, Captured.DifficultyLevel, Captured.EnemyTags);
    }
}

int32_t UFortAIEncounterInfo::GetSpawnGroupCostCall(UFortAISpawnGroup* SpawnGroup, const FFortAISpawnGroupUpgrade* Upgrade, float DifficultyLevel, int32_t NumPlayers)
{
    if (Capture)
    {
        Capture->Upgrade = Upgrade;
        Capture->DifficultyLevel = DifficultyLevel;
    }
    return Natives::UFortAISpawnGroup_GetSpawnGroupCost(SpawnGroup, Upgrade, DifficultyLevel, NumPlayers);
}

void UFortAIEncounterInfo::PickPawnUpgradeCall(const FFortAISpawnGroupUpgrade* Upgrade, const FGameplayTagContainer& EnemyTags, int32_t NumPlayers, void* OutPawnUpgrade)
{
    if (Capture)
    {
        Capture->EnemyTags.AppendTags(EnemyTags);
    }
    Natives::FFortAISpawnGroupUpgrade_PickPawnUpgrade(Upgrade, &EnemyTags, NumPlayers, OutPawnUpgrade);
}
