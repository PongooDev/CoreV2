#pragma once

#include "Containers/Map.h"
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
    // One tag up to 4.2; 4.4 (4.3?) makes it a container, RequiredGameplayTags, next to a new GameplayTags.
    DefineUProperty(FGameplayTag, RequiredGameplayTag)
    DefineUProperty(FGameplayTagContainer, RequiredGameplayTags)
    DefineUProperty(bool, bAllowBonusLootDrops)

    static bool HasRequiredGameplayTag() { return Member_RequiredGameplayTag.IsAvailable(); }
    static bool HasRequiredGameplayTags() { return Member_RequiredGameplayTags.IsAvailable(); }
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
    // 7.x (7.10 +0x14): the name a picker's NamedWeightsMap multiplies the row's weight by (UFortKismetLibrary::
    // PickLootDropsWithNamedWeights; Frostnite's supply drops pass TrapPatrolWard 0, so no Patrol Ward trap drops).
    DefineUProperty(FName, NamedWeightMult)

    static bool HasNamedWeightMult() { return Member_NamedWeightMult.IsAvailable(); }
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

// Transient. LootTier is at +0; LootTierKey follows it, padded to +8 while FName is 8-byte aligned (to UE 4.21, as 6.31
// reflects it) and right after at +4 from UE 4.22 (7.00, CL-4629139: 0xC bytes, as 21.40). So the key is read where the
// build's reflection puts it, and at +8 where it isn't reflected.
struct FChosenQuotaInfo
{
    DefineUnrealStruct(FChosenQuotaInfo, FortniteGame)

public:
    bool IsSet() const { return !GetLootTierKey().IsNone(); }
    FName GetLootTierKey() const;

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
    // NamedWeightsMap (7.x): each package row whose NamedWeightMult it names has its weight multiplied by that value, in
    // every package the pick calls into. Null: the rows' own weights.
    static bool PickLootDrops(TStructArray<FFortItemEntry>& OutLootToDrop, int32_t WorldLevel, FName LootTierKey, UAbilitySystemComponent* AbilitySystemComponent, const FGameplayTagContainer& ContextTags, bool bAllowBonusDrops, bool bPreview, const TMap<FName, float>* NamedWeightsMap = nullptr);
    static void PickLootDropsFromLootPackage(TStructArray<FFortItemEntry>& OutLootToDrop, FName LootPackageName, int32_t WorldLevel, int32_t LootPackageCategory, UAbilitySystemComponent* AbilitySystemComponent, const FGameplayTagContainer& ContextTags, bool bAllowBonusDrops, const TMap<FName, float>* NamedWeightsMap = nullptr);
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
