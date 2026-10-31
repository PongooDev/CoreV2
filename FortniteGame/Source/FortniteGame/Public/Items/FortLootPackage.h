#pragma once

#include "Containers/UnrealString.h"
#include "GameplayTagContainer.h"
#include "StructArray.h"
#include "UObject/AssetPtr.h"
#include "UObject/Object.h"

#include <utility>
#include <vector>

class UAbilitySystemComponent;
class UFortItemDefinition;
struct FFortBuildingRandomList;
struct FFortItemEntry;
struct FFortTileLootData;

class ELootQuotaLevel
{
    DefineUnrealEnum(ELootQuotaLevel, FortniteGame)
    DefineUEnumValue(Unlimited)
    DefineUEnumValue(NumLevels)
};

struct FFortLootTierData
{
    DefineUnrealStruct(FFortLootTierData, FortniteGame)

public:
    DefineUProperty(FName, TierGroup)
    DefineUProperty(float, Weight)
    DefineUProperty(ELootQuotaLevel, QuotaLevel)
    DefineUProperty(int32_t, LootTier)
    DefineUProperty(int32_t, MinWorldLevel)
    DefineUProperty(int32_t, MaxWorldLevel)
    DefineUProperty(FString, StreakBreakerCurrency)
    DefineUProperty(int32_t, StreakBreakerPointsMin)
    DefineUProperty(int32_t, StreakBreakerPointsMax)
    DefineUProperty(int32_t, StreakBreakerPointsSpend)
    DefineUProperty(FName, LootPackage)
    DefineUProperty(FName, LootPreviewPackage)
    DefineUProperty(float, NumLootPackageDrops)
    DefineUProperty(TArray<int32_t>, LootPackageCategoryWeightArray)
    DefineUProperty(TArray<int32_t>, LootPackageCategoryMinArray)
    DefineUProperty(TArray<int32_t>, LootPackageCategoryMaxArray)
    DefineUProperty(FGameplayTag, RequiredGameplayTag)
    DefineUProperty(bool, bAllowBonusLootDrops)
};

struct FFortLootPackageData
{
    DefineUnrealStruct(FFortLootPackageData, FortniteGame)

public:
    DefineUProperty(FName, LootPackageID)
    DefineUProperty(float, Weight)
    DefineUProperty(int32_t, Count)
    DefineUProperty(int32_t, LootPackageCategory)
    DefineUProperty(FName, RequiredTag)
    DefineUProperty(FString, LootPackageCall)
    DefineUProperty(TAssetPtr<UFortItemDefinition>, ItemDefinition)
    DefineUProperty(FString, PersistentLevel)
    DefineUProperty(int32_t, MinWorldLevel)
    DefineUProperty(int32_t, MaxWorldLevel)
    DefineUProperty(bool, bAllowBonusDrops)
};

struct FFortLootQuotaData
{
    DefineUnrealStruct(FFortLootQuotaData, FortniteGame)

public:
    DefineUProperty(FName, QuotaCategory)
    DefineUProperty(ELootQuotaLevel, QuotaLevel)
    DefineUProperty(int32_t, Min)
    DefineUProperty(int32_t, Max)
    DefineUProperty(float, Quota)
    DefineUProperty(int32_t, MinWorldLevel)
    DefineUProperty(int32_t, MaxWorldLevel)

    static void GetLootQuotasForCategory(const FName& QuotaCategory, FFortTileLootData& OutLootData, int32_t WorldLevel);
};

// Transient and without reflected members; the layout is read from the code that fills and reads it.
struct FChosenQuotaInfo
{
    DefineUnrealStruct(FChosenQuotaInfo, FortniteGame)

public:
    bool IsSet() const { return !LootTierKey.IsNone(); }

    int32_t LootTier = -1;
    int32_t Pad = 0;
    FName LootTierKey;
};

static_assert(sizeof(FChosenQuotaInfo) == 0x10);

struct FFortLootTierRow
{
    FName LootTierKey;
    FFortLootTierData* LootTierData = nullptr;
};

class UFortLootPackage
{
public:
    static bool PickLootDrops(TStructArray<FFortItemEntry>& OutLootToDrop, int32_t WorldLevel, FName LootTierKey, UAbilitySystemComponent* AbilitySystemComponent, const FGameplayTagContainer& ContextTags, bool bAllowBonusDrops, bool bPreview);
    static void PickLootDropsFromLootPackage(TStructArray<FFortItemEntry>& OutLootToDrop, FName LootPackageName, int32_t WorldLevel, int32_t LootPackageCategory, UAbilitySystemComponent* AbilitySystemComponent, const FGameplayTagContainer& ContextTags, bool bAllowBonusDrops);
};

class UFortLootTier
{
public:
    static int32_t FindLootTierFromKey(FName LootTierKey);
    static void GetLootTierRows(std::vector<FFortLootTierRow>& OutRows, float& OutTotalWeight, FName TierGroupName, int32_t WorldLevel, int32_t LootTier, const FGameplayTagContainer& ContextTags, uint8_t QuotaLevel);
    static bool PickLootTierFromRows(FName& OutLootTierKey, int32_t& OutLootTier, float TotalWeight, const std::vector<FFortLootTierRow>& Rows, FFortBuildingRandomList* RandomList);
    static bool PickLootTierForQuotaLevel(FName& OutLootTierKey, int32_t& OutLootTier, const std::vector<FFortLootTierRow>& Rows, uint8_t QuotaLevel, FFortBuildingRandomList* RandomList);
    static bool ShouldDropLoot(FName LootTierKey, const wchar_t* ContextString);
    static bool GetLootPackageCategoryCounts(const FFortLootTierData& TierData, int32_t NumLootDrops, std::vector<std::pair<int32_t, int32_t>>& OutCategoryCounts);
};

class UFortLootLevel
{
public:
    static int32_t GetItemLevel(const void* LootLevelData, int32_t WorldLevel);
};
