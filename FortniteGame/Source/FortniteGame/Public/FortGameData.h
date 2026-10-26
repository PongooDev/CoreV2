#pragma once

#include "GameplayTagContainer.h"
#include "Items/FortQuickBars.h"
#include "StructArray.h"
#include "UObject/AssetPtr.h"
#include "UObject/Object.h"

#include <vector>

class AActor;
class AFortAIDirector;
class UCurveVector;
class UDataTable;
class UFortResourceItemDefinition;
struct FFortLootTierData;
class UEnvQuery;
class UFortAbilitySet;
class UFortHomebaseManager;
class UFortItemDefinition;
class UFortWorldItemDefinition;

struct FItemDefinitionAndCount
{
    DefineUnrealStruct(FItemDefinitionAndCount, FortniteGame)
    DefineUProperty(int32_t, Count)
    DefineUProperty(TAssetPtr<UFortItemDefinition>, ItemDefinition)
};

struct FFortPlayerSpawnPadPlacementData
{
    DefineUnrealStruct(FFortPlayerSpawnPadPlacementData, FortniteGame)
    DefineUProperty(UEnvQuery*, PlacementQuery)
    DefineUProperty(TAssetSubclassOf<AActor>, ActorToPlace)
    DefineBitfieldUProperty(bSnapToGrid)
    DefineBitfieldUProperty(bAdjustPlacementForFloors)
    DefineUProperty(FGameplayTagContainer, TagsToAddToChosenPlacementActor)
};

struct FConditionalFoundationQuota
{
    DefineUnrealStruct(FConditionalFoundationQuota, FortniteGame)
};

class UFortGameData : public UObject
{
    DefineUnrealClass(UFortGameData, FortniteGame)

public:
    DefineUStructProperty(FQuickBarData, QuickBarDefinitions)
    DefineUProperty(TArray<TAssetPtr<UFortWorldItemDefinition>>, HarvestingTools)
    DefineUStructProperty(FFortPlayerSpawnPadPlacementData, DefaultPlayerSpawnPadPlacementData)
    DefineUProperty(TAssetPtr<UFortAbilitySet>, GenericPlayerAbilitySet)
    DefineUStructArrayProperty(FItemDefinitionAndCount, DefaultInventoryList)
    DefineUProperty(TArray<TAssetSubclassOf<AFortAIDirector>>, AIDirectors)
    DefineUProperty(UClass*, SharedMissionListsClass)
    DefineUProperty(TAssetPtr<UFortHomebaseManager>, HomebaseManager)
    DefineUStructProperty(FConditionalFoundationQuota, ConditionalFoundationQuota)
    DefineUProperty(TArray<TAssetPtr<UDataTable>>, LootTierDataTables)
    DefineUProperty(TArray<TAssetPtr<UDataTable>>, LootPackageDataTables)
    DefineUProperty(UDataTable*, LootQuotaData)
    DefineUProperty(float, PickupSplineRandomMax)
    DefineUProperty(UCurveVector*, PickupSplineCurve)

    std::vector<UDataTable*> GetLootTierDataTables();
    std::vector<UDataTable*> GetLootPackageDataTables();
    bool HasLootPackageDataTables();
    UDataTable* GetLootQuotaData();
    FFortLootTierData* FindLootTierDataRow(FName LootTierKey, const wchar_t* ContextString);

    static ptrdiff_t ConditionalFoundationQuotaOffset() { return Member_ConditionalFoundationQuota.GetOffsetChecked(); }

    const FQuickBarData& GetQuickBarDefinition(EFortQuickBars InQuickBar) const
    {
        const FQuickBarData& Definitions = QuickBarDefinitions;
        const uint8_t* Definition = reinterpret_cast<const uint8_t*>(&Definitions) + static_cast<size_t>(InQuickBar) * FQuickBarData::StaticStruct()->GetStructureSize();
        return *reinterpret_cast<const FQuickBarData*>(Definition);
    }
};
