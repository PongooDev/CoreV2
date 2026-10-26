#include "pch.h"
#include "Items/FortLootPackage.h"
#include "AbilitySystemBlueprintLibrary.h"
#include "AbilitySystemComponent.h"
#include "Abilities/FortPlayerAttrSet.h"
#include "Engine/DataTable.h"
#include "Finder.h"
#include "FortAssetManager.h"
#include "FortGameData.h"
#include "Items/FortItem.h"
#include "Items/FortItemDefinition.h"
#include "Logging/LogMacros.h"
#include "Math/UnrealMathUtility.h"
#include "Natives.h"
#include "Templates/Casts.h"
#include "UObject/UnrealType.h"

#include <cmath>
#include <format>
#include <string>
#include <vector>

DEFINE_LOG_CATEGORY_STATIC(LogFortLoot, Log, All);

namespace
{
    UFortGameData* GetGameData()
    {
        return UFortAssetManager::Get().GetGameData();
    }

    bool PackageRowMatches(const FFortLootPackageData& Row, FName LootPackageName, int32_t LootPackageCategory, const FGameplayTagContainer& ContextTags, int32_t WorldLevel)
    {
        if (Row.LootPackageID != LootPackageName || (LootPackageCategory != -1 && Row.LootPackageCategory != LootPackageCategory))
        {
            return false;
        }
        const FName RequiredTag = Row.RequiredTag;
        if (!RequiredTag.IsNone() && !ContextTags.HasTag(FGameplayTag{ RequiredTag }))
        {
            return false;
        }
        if (WorldLevel >= 0)
        {
            if (Row.MaxWorldLevel >= 0 && WorldLevel > Row.MaxWorldLevel)
            {
                return false;
            }
            if (Row.MinWorldLevel >= 0 && WorldLevel < Row.MinWorldLevel)
            {
                return false;
            }
        }
        return true;
    }

    const FGameplayAttribute& PlayerAttribute(const wchar_t* Name, FGameplayAttribute& Cached)
    {
        if (!Cached.IsValid() && UFortPlayerAttrSet::StaticClass())
        {
            Cached = FGameplayAttribute(UFortPlayerAttrSet::StaticClass()->FindPropertyByName(FName(Name)));
        }
        return Cached;
    }

    // DoubleItemLootChance and DoubleLootRollChance, evaluated on the looter with the given source tags.
    bool RollBonusChance(UAbilitySystemComponent* AbilitySystemComponent, const FGameplayAttribute& Attribute, const FGameplayTagContainer& SourceTags)
    {
        bool bSuccess = false;
        const float Chance = UAbilitySystemBlueprintLibrary::EvaluateAttributeValueWithTags(AbilitySystemComponent, Attribute, SourceTags, FGameplayTagContainer(), bSuccess);
        if (!bSuccess)
        {
            return false;
        }
        const float Roll = FMath::FRand();
        return Chance > 0.0f && Chance >= Roll;
    }
}

int32_t UFortLootLevel::GetItemLevel(const void* LootLevelData, int32_t WorldLevel)
{
    if (!Natives::UFortLootLevel_GetItemLevel && !Finder::ResolveLootLive())
    {
        return 0;
    }
    return Natives::UFortLootLevel_GetItemLevel(LootLevelData, WorldLevel);
}

int32_t UFortLootTier::FindLootTierFromKey(FName LootTierKey)
{
    UFortGameData* GameData = GetGameData();
    const FFortLootTierData* TierData = GameData ? GameData->FindLootTierDataRow(LootTierKey, L"UFortLootTier::FindLootTierFromKey") : nullptr;
    return TierData ? static_cast<int32_t>(TierData->LootTier) : -1;
}

bool UFortLootTier::ShouldDropLoot(FName LootTierKey, const wchar_t* ContextString)
{
    if (LootTierKey.IsNone())
    {
        return false;
    }
    UFortGameData* GameData = GetGameData();
    const FFortLootTierData* TierData = GameData ? GameData->FindLootTierDataRow(LootTierKey, ContextString) : nullptr;
    if (!TierData)
    {
        return false;
    }
    const float Roll = FMath::FRand();
    const float NumLootPackageDrops = TierData->NumLootPackageDrops;
    return NumLootPackageDrops > 0.0f && Roll <= NumLootPackageDrops;
}

bool UFortLootTier::GetLootPackageCategoryCounts(const FFortLootTierData& TierData, int32_t NumLootDrops, std::vector<std::pair<int32_t, int32_t>>& OutCategoryCounts)
{
    OutCategoryCounts.clear();
    const TArray<int32_t>& Weights = TierData.LootPackageCategoryWeightArray;
    const TArray<int32_t>& Mins = TierData.LootPackageCategoryMinArray;
    const TArray<int32_t>& Maxs = TierData.LootPackageCategoryMaxArray;
    if (Mins.Num() != Weights.Num() || Mins.Num() != Maxs.Num())
    {
        return false;
    }

    const auto FindCount = [&OutCategoryCounts](int32_t Category) -> int32_t*
    {
        for (auto& [Key, Count] : OutCategoryCounts)
        {
            if (Key == Category)
            {
                return &Count;
            }
        }
        return nullptr;
    };

    int32_t NumDropped = 0;
    for (int32_t Category = 0; Category < Mins.Num(); ++Category)
    {
        if (Mins[Category] > 0)
        {
            OutCategoryCounts.emplace_back(Category, Mins[Category]);
            NumDropped += Mins[Category];
        }
    }
    if (NumDropped > NumLootDrops)
    {
        UE_LOG(LogFortLoot, Warning, TEXT("Requested %i loot drops but minimum drops is %i for loot package %s"), NumLootDrops, NumDropped, *TierData.LootPackage.ToString());
    }

    std::vector<std::pair<int32_t, int32_t>> CategoryWeights;
    int32_t TotalWeight = 0;
    for (int32_t Category = 0; Category < Weights.Num(); ++Category)
    {
        const int32_t Weight = Weights[Category];
        if (Weight <= 0)
        {
            continue;
        }
        const int32_t Max = Maxs[Category];
        const int32_t* Count = FindCount(Category);
        if (Max < 0 || (Count ? *Count : 0) < Max)
        {
            TotalWeight += Weight;
            CategoryWeights.emplace_back(Category, Weight);
        }
    }

    if (NumDropped < NumLootDrops)
    {
        while (TotalWeight > 0)
        {
            int32_t Pick = FMath::RoundToInt(static_cast<float>(TotalWeight) * FMath::FRand());
            size_t Selected = 0;
            while (Selected + 1 < CategoryWeights.size() && Pick > CategoryWeights[Selected].second)
            {
                Pick -= CategoryWeights[Selected].second;
                ++Selected;
            }

            const int32_t Category = CategoryWeights[Selected].first;
            const int32_t Max = Maxs[Category];
            int32_t* Count = FindCount(Category);
            if (!Count)
            {
                OutCategoryCounts.emplace_back(Category, 0);
                Count = &OutCategoryCounts.back().second;
            }
            ++*Count;
            ++NumDropped;

            if (Max >= 0)
            {
                if (*Count > Max)
                {
                    UE_LOG(LogFortLoot, Warning, TEXT("We have dropped %i items of category %i but the max is %i for tier group %s at tier %i."), *Count, Category, Max, *TierData.TierGroup.ToString(), static_cast<int32_t>(TierData.LootTier));
                }
                if (*Count >= Max)
                {
                    TotalWeight -= CategoryWeights[Selected].second;
                    CategoryWeights.erase(CategoryWeights.begin() + Selected);
                }
            }

            if (NumDropped >= NumLootDrops)
            {
                return true;
            }
        }
        if (NumDropped < NumLootDrops)
        {
            UE_LOG(LogFortLoot, Warning, TEXT("Requested %i loot drops but maximum drops is %i for loot package %s"), NumLootDrops, NumDropped, *TierData.LootPackage.ToString());
        }
    }
    return true;
}

bool UFortLootPackage::PickLootDrops(TStructArray<FFortItemEntry>& OutLootToDrop, int32_t WorldLevel, FName LootTierKey, UAbilitySystemComponent* AbilitySystemComponent, const FGameplayTagContainer& ContextTags, bool bAllowBonusDrops, bool bPreview)
{
    if (LootTierKey.IsNone())
    {
        return false;
    }

    UFortGameData* GameData = GetGameData();
    const FFortLootTierData* TierData = GameData ? GameData->FindLootTierDataRow(LootTierKey, L"UFortLootPackage::PickLootDrops") : nullptr;
    if (!TierData || !GameData->HasLootPackageDataTables())
    {
        return false;
    }

    const int32_t NumBefore = OutLootToDrop.Num();
    const FName LootPackage = bPreview ? static_cast<FName>(TierData->LootPreviewPackage) : static_cast<FName>(TierData->LootPackage);
    const float NumLootPackageDrops = TierData->NumLootPackageDrops;
    UE_LOG(LogFortLoot, Verbose, TEXT("Selecting %f drops from %s"), NumLootPackageDrops, *LootPackage.ToString());
    if (!LootPackage.IsNone() && NumLootPackageDrops > 0.0f)
    {
        int32_t NumLootDrops = 1;
        if (!bPreview && NumLootPackageDrops >= 1.0f)
        {
            NumLootDrops = FMath::FloorToInt(NumLootPackageDrops);
            const float Fraction = NumLootPackageDrops - static_cast<float>(NumLootDrops);
            if (Fraction > 0.00001f)
            {
                NumLootDrops += Fraction >= FMath::FRand() ? 1 : 0;
            }
        }
        UE_LOG(LogFortLoot, Verbose, TEXT("Actual number of loot drops is: %i"), NumLootDrops);

        std::vector<std::pair<int32_t, int32_t>> CategoryCounts;
        if (bPreview)
        {
            CategoryCounts.emplace_back(0, 1);
        }
        else
        {
            UFortLootTier::GetLootPackageCategoryCounts(*TierData, NumLootDrops, CategoryCounts);
        }

        const bool bAllowBonus = !bPreview && bAllowBonusDrops && TierData->bAllowBonusLootDrops;
        for (const auto& [Category, Count] : CategoryCounts)
        {
            UE_LOG(LogFortLoot, Verbose, TEXT("Dropping %i loot packages from drop category %i"), Count, Category);
            for (int32_t Drop = 0; Drop < Count; ++Drop)
            {
                PickLootDropsFromLootPackage(OutLootToDrop, LootPackage, WorldLevel, Category, AbilitySystemComponent, ContextTags, bAllowBonus);
            }
        }
    }

    const bool bDroppedAny = OutLootToDrop.Num() > NumBefore;

    // Stack drops of items that don't allow multiple stacks.
    for (int32_t Index = 0; Index < OutLootToDrop.Num(); ++Index)
    {
        FFortItemEntry& Entry = OutLootToDrop[Index];
        UFortItemDefinition* ItemDefinition = Entry.ItemDefinition;
        if (!ItemDefinition || ItemDefinition->bAllowMultipleStacks)
        {
            continue;
        }
        for (int32_t Other = Index + 1; Other < OutLootToDrop.Num(); ++Other)
        {
            FFortItemEntry& OtherEntry = OutLootToDrop[Other];
            if (static_cast<UFortItemDefinition*>(OtherEntry.ItemDefinition) != ItemDefinition)
            {
                continue;
            }
            const int32_t NewCount = FMath::Max(Entry.Count + OtherEntry.Count, 0);
            if (NewCount != Entry.Count)
            {
                Entry.Count = NewCount;
                Entry.bIsDirty = true;
            }
            OutLootToDrop.RemoveAt(Other--);
        }
    }
    return bDroppedAny;
}

void UFortLootPackage::PickLootDropsFromLootPackage(TStructArray<FFortItemEntry>& OutLootToDrop, FName LootPackageName, int32_t WorldLevel, int32_t LootPackageCategory, UAbilitySystemComponent* AbilitySystemComponent, const FGameplayTagContainer& ContextTags, bool bAllowBonusDrops)
{
    UFortGameData* GameData = GetGameData();
    if (!GameData)
    {
        return;
    }

    std::vector<FFortLootPackageData*> Rows;
    float TotalWeight = 0.0f;
    for (UDataTable* Table : GameData->GetLootPackageDataTables())
    {
        UScriptStruct* RowStruct = Table->RowStruct;
        if (!RowStruct || !RowStruct->IsChildOf(FFortLootPackageData::StaticStruct()))
        {
            continue;
        }
        for (auto& Pair : Table->GetRowMap())
        {
            FFortLootPackageData* Row = reinterpret_cast<FFortLootPackageData*>(Pair.Value);
            if (Row && PackageRowMatches(*Row, LootPackageName, LootPackageCategory, ContextTags, WorldLevel))
            {
                Rows.push_back(Row);
                TotalWeight += Row->Weight;
            }
        }
    }

    if (TotalWeight == 0.0f)
    {
        std::wstring Message = std::format(L"Loot Package {} has no valid weights with the following data:\nLootPackageCategory: {}\nWorldLevel: {}\nTags: ", LootPackageName.ToStdString(), LootPackageCategory, WorldLevel);
        const TArray<FGameplayTag>& Tags = ContextTags.GameplayTags;
        for (const FGameplayTag& Tag : Tags)
        {
            Message += Tag.TagName.ToStdString() + L" ";
        }
        UE_LOG(LogFortLoot, Warning, TEXT("%s"), Message.c_str());
    }

    float Roll = FMath::FRand() * TotalWeight;
    FFortLootPackageData* Selected = nullptr;
    for (FFortLootPackageData* Row : Rows)
    {
        const float Weight = Row->Weight;
        if (Roll <= Weight)
        {
            Selected = Row;
            break;
        }
        Roll -= Weight;
    }

    if (Selected)
    {
        UE_LOG(LogFortLoot, Verbose, TEXT("PickLootDropsFromLootPackage selected package %s"), *Selected->LootPackageID.ToString());

        const FString& LootPackageCall = Selected->LootPackageCall;
        if (LootPackageCall.Len() > 0)
        {
            const int32_t Count = Selected->Count;
            const FName CalledPackage(*LootPackageCall);
            for (int32_t Call = 0; Call < Count; ++Call)
            {
                PickLootDropsFromLootPackage(OutLootToDrop, CalledPackage, WorldLevel, -1, AbilitySystemComponent, ContextTags, false);
            }
        }
        else if (Selected->ItemDefinition.ToStringReference().IsNull())
        {
            UE_LOG(LogFortLoot, Warning, TEXT("Loot Package %s does not contain a LootPackageCall or ItemDefinition."), *Selected->LootPackageID.ToString());
        }
        else if (UFortItemDefinition* ItemDefinition = Selected->ItemDefinition.LoadSynchronous())
        {
            int32_t CountMultiplier = 1;
            if (Selected->bAllowBonusDrops && AbilitySystemComponent)
            {
                static FGameplayAttribute DoubleItemLootChance;
                FGameplayTagContainer SourceTags;
                SourceTags.AppendTags(ItemDefinition->GameplayTags);
                SourceTags.AppendTags(ContextTags);
                AbilitySystemComponent->GetOwnedGameplayTags(SourceTags);
                if (RollBonusChance(AbilitySystemComponent, PlayerAttribute(L"DoubleItemLootChance", DoubleItemLootChance), SourceTags))
                {
                    CountMultiplier = 2;
                }
            }

            int32_t ItemLevel = 0;
            if (UFortWorldItemDefinition* WorldItemDefinition = Cast<UFortWorldItemDefinition>(ItemDefinition))
            {
                ItemLevel = UFortLootLevel::GetItemLevel(WorldItemDefinition->GetLootLevelData(), WorldLevel);
            }
            UE_LOG(LogFortLoot, Verbose, TEXT("Dropping %s (Count: %i, CountMultiplier: %i, ItemLevel: %i"), *ItemDefinition->GetName(), static_cast<int32_t>(Selected->Count), CountMultiplier, ItemLevel);

            int32_t Remaining = Selected->Count * CountMultiplier;
            const int32_t MaxStackSize = FMath::Max(static_cast<int32_t>(ItemDefinition->MaxStackSize), 1);
            while (Remaining > 0)
            {
                const int32_t StackCount = FMath::Min(Remaining, MaxStackSize);
                FFortItemEntry& Entry = OutLootToDrop.AddDefaulted();
                UScriptStruct* EntryStruct = FFortItemEntry::StaticStruct();
                EntryStruct->DestroyStruct(&Entry);
                FFortItemEntry::Construct(&Entry, ItemDefinition, StackCount, FMath::Max(ItemLevel, 0));
                Remaining -= StackCount;
            }
        }
        else
        {
            UE_LOG(LogFortLoot, Warning, TEXT("Failed to load loot package %s."), *Selected->LootPackageID.ToString());
        }
    }

    if (bAllowBonusDrops && AbilitySystemComponent)
    {
        static FGameplayAttribute DoubleLootRollChance;
        FGameplayTagContainer SourceTags;
        AbilitySystemComponent->GetOwnedGameplayTags(SourceTags);
        SourceTags.AppendTags(ContextTags);
        if (RollBonusChance(AbilitySystemComponent, PlayerAttribute(L"DoubleLootRollChance", DoubleLootRollChance), SourceTags))
        {
            PickLootDropsFromLootPackage(OutLootToDrop, LootPackageName, WorldLevel, LootPackageCategory, AbilitySystemComponent, ContextTags, false);
        }
    }
}
