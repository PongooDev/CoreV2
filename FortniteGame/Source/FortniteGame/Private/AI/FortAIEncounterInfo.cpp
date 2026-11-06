#include "pch.h"
#include "AI/FortAIEncounterInfo.h"
#include "Finder.h"
#include "FortKismetLibrary.h"
#include "Hooks.h"
#include "Items/FortItem.h"
#include "Items/FortLootPackage.h"
#include "Natives.h"
#include "Pawns/FortAIPawn.h"
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

void UFortAISpawnGroup::GetEnemyGameplayTags(int32_t EnemyIndex, FGameplayTagContainer& OutTags) const
{
    TStructArray<FSpawnGroupEnemy>& Enemies = EnemiesToSpawn;
    UClass* VariantClass = EnemyIndex >= 0 && EnemyIndex < Enemies.Num() ? static_cast<UClass*>(Enemies[EnemyIndex].EnemyVariantClass) : nullptr;
    const UFortAIPawnVariant* Variant = VariantClass && VariantClass->IsChildOf(UFortAIPawnVariant::StaticClass()) ? static_cast<const UFortAIPawnVariant*>(VariantClass->GetDefaultObject(false)) : nullptr;
    if (!Variant)
    {
        return;
    }
    TStructArray<FFortAIPawnVariantDefinition>& Definitions = Variant->PawnVariantDefinitions;
    for (int32_t Index = 0; Index < Definitions.Num(); ++Index)
    {
        UClass* PawnClass = Definitions[Index].PawnClass;
        if (PawnClass && PawnClass->IsChildOf(AFortAIPawn::StaticClass()))
        {
            if (const AFortPawn* PawnDefault = static_cast<const AFortPawn*>(PawnClass->GetDefaultObject(false)))
            {
                OutTags.AppendTags(PawnDefault->GameplayTags);
            }
            return;
        }
    }
}

void UFortAIEncounterInfo::GetAIPawnLootDropData(const FGameplayTagContainer& AITags, FFortAIPawnLootDropData& OutLootDropData) const
{
    // Each row's tags must all be among the pawn's (its tags or their parents). The rows are walked last to first, and
    // a later match only replaces the one found when its priority is higher.
    const auto HasAllTags = [&AITags](const FGameplayTagContainer& RowTags)
    {
        for (const FGameplayTag& Tag : RowTags.GameplayTags)
        {
            if (!AITags.HasTag(Tag))
            {
                return false;
            }
        }
        return true;
    };

    TStructArray<FFortAIBaseLootDropRow>& BaseRows = BaseLootDropRows;
    const FFortAIBaseLootDropRow* BaseRow = nullptr;
    for (int32_t Index = BaseRows.Num() - 1; Index >= 0; --Index)
    {
        const FFortAIBaseLootDropRow& Row = BaseRows[Index];
        if (!HasAllTags(Row.AITagsContainer))
        {
            continue;
        }
        if (!BaseRow || static_cast<int32_t>(BaseRow->Priority) < static_cast<int32_t>(Row.Priority))
        {
            BaseRow = &Row;
        }
    }
    if (!BaseRow)
    {
        return;
    }

    TStructArray<FFortAILootDropModifierRow>& ModifierRows = LootDropModifierRows;
    const FFortAILootDropModifierRow* ModifierRow = nullptr;
    for (int32_t Index = ModifierRows.Num() - 1; Index >= 0; --Index)
    {
        const FFortAILootDropModifierRow& Row = ModifierRows[Index];
        if (!HasAllTags(Row.AITagsContainer))
        {
            continue;
        }
        if (!ModifierRow || static_cast<int32_t>(ModifierRow->Priority) < static_cast<int32_t>(Row.Priority))
        {
            ModifierRow = &Row;
        }
    }

    OutLootDropData.WorldItemTierGroup = BaseRow->WorldItemLootTierGroup;
    OutLootDropData.AccountItemTierGroup = BaseRow->AccountItemLootTierGroup;
    const float WorldMultiplier = ModifierRow ? static_cast<float>(ModifierRow->WorldItemDropChanceMultiplier) : 1.0f;
    const float AccountMultiplier = ModifierRow ? static_cast<float>(ModifierRow->AccountItemDropChanceMultiplier) : 1.0f;
    OutLootDropData.WorldItemLootDropChance = FMath::Clamp(WorldMultiplier * static_cast<float>(BaseRow->WorldItemDropChance), 0.0f, 1.0f);
    OutLootDropData.AccountItemLootDropChance = FMath::Clamp(AccountMultiplier * static_cast<float>(BaseRow->AccountItemDropChance), 0.0f, 1.0f);
}

void UFortAIEncounterInfo::GetAIPawnLootDropTags(const AFortAIPawn* Pawn, FGameplayTagContainer& OutAITags) const
{
    // The queue (4.0 0x8DABE0) takes the encounter's tags (0x8C32D0, of which only its own ModifierTags and
    // GameContextTags are reflected; the director's and the mutators' tags it adds there aren't kept, and none of 4.0's
    // loot rows asks for an encounter tag), then the group instance's UpgradeTags, its spawn group's tags and the enemy's.
    OutAITags.AppendTags(ModifierTags);
    OutAITags.AppendTags(GameContextTags);

    const FGuid GroupGuid = Pawn->SpawnGroupGuid;
    TStructArray<FSpawnGroupInstanceInfo>& Groups = ActiveSpawnGroups;
    const FSpawnGroupInstanceInfo* Instance = nullptr;
    for (int32_t Index = 0; Index < Groups.Num(); ++Index)
    {
        if (Groups[Index].GroupGuid == GroupGuid)
        {
            Instance = &Groups[Index];
        }
    }
    UFortAISpawnGroup* SpawnGroup = Instance ? static_cast<UFortAISpawnGroup*>(Instance->SpawnGroup) : nullptr;
    if (!SpawnGroup)
    {
        return;
    }
    OutAITags.AppendTags(Instance->UpgradeTags);
    OutAITags.AppendTags(SpawnGroup->SpawnGroupGameplayTags);
    SpawnGroup->GetEnemyGameplayTags(Pawn->EnemyIndexInSpawnGroup, OutAITags);
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
    // 1.10 holds FFortAISpawnGroupUpgradeData instead; the roll this restores was cut on 1.2 only, so its game keeps its own.
    if (!Member_AvailableUpgrades.IsAvailable())
    {
        Log::Info(L"UFortAIEncounterInfo: AvailableUpgrades isn't FFortAISpawnGroupUpgrade in this build, spawn group upgrades are the game's own");
        return;
    }

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
