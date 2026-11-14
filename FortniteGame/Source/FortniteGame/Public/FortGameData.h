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
class UCustomCharacterPart;
class UDataTable;
class UFortResourceItemDefinition;
struct FFortLootTierData;
class UEnvQuery;
class UFortAbilitySet;
class UFortHomebaseManager;
class UGameplayAbility;
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
    DefineUProperty(TArray<TAssetPtr<UFortWorldItemDefinition>>, CheatInventoryItems)
    DefineUStructProperty(FFortPlayerSpawnPadPlacementData, DefaultPlayerSpawnPadPlacementData)
    DefineUProperty(TAssetPtr<UFortAbilitySet>, GenericPlayerAbilitySet)
    // The ability an emote plays through (its montage comes from the spec's source object, the emote item).
    DefineUProperty(TAssetSubclassOf<UGameplayAbility>, EmoteGameplayAbility)
    // 4.0: the ability a spray plays through (GAB_Spray_Generic; the spray is the spec's source object).
    DefineUProperty(TAssetSubclassOf<UGameplayAbility>, SprayGameplayAbility)
    DefineUProperty(TAssetPtr<UFortWorldItemDefinition>, EditToolItem)
    DefineUStructArrayProperty(FItemDefinitionAndCount, DefaultInventoryList)
    DefineUProperty(TArray<TAssetSubclassOf<AFortAIDirector>>, AIDirectors)
    DefineUProperty(UClass*, SharedMissionListsClass)

private:
    // 4.2 keeps SharedMissionListsClass as a soft class pointer (a TSoftClassPtr) where earlier builds hold the class.
    static inline Reflection::FMemberInfo Member_SharedMissionListsClassPath{ &StaticReflectionOwner, TEXT("SharedMissionListsClass"), sizeof(TAssetSubclassOf<UObject>), Reflection::EMemberKind::Value, nullptr, Reflection::SoftPathGrowth<TAssetSubclassOf<UObject>> };

public:
    // The shared mission lists' class, loading it from its path on 4.2+; null when the game data names none.
    UClass* GetSharedMissionListsClass() const;

    DefineUProperty(TAssetPtr<UFortHomebaseManager>, HomebaseManager)
    DefineUStructProperty(FConditionalFoundationQuota, ConditionalFoundationQuota)
    DefineUProperty(TArray<TAssetPtr<UDataTable>>, LootTierDataTables)
    DefineUProperty(TArray<TAssetPtr<UDataTable>>, LootPackageDataTables)
    DefineUProperty(UDataTable*, LootQuotaData)
    DefineUProperty(float, PickupSplineRandomMax)
    DefineUProperty(UCurveVector*, PickupSplineCurve)
    DefineUProperty(TAssetPtr<UCustomCharacterPart>, DefaultBackpackCharacterPart)
    DefineUProperty(UFortWorldItemDefinition*, StoneItemDefinition)
    DefineUProperty(UFortWorldItemDefinition*, WoodItemDefinition)
    DefineUProperty(UFortWorldItemDefinition*, MetalItemDefinition)
    DefineUProperty(UFortWorldItemDefinition*, PermaniteItemDefinition)
    // Save the World's BluGlo (BluGloWorldItemData, a never-persist world item).
    DefineUProperty(UFortWorldItemDefinition*, BluGloItemDefinition)

private:
    // 6.30 (CL-4573096) keeps these as soft pointers (TSoftObjectPtr) where earlier builds hold the object, like
    // SharedMissionListsClass on 4.2. The getters below read whichever the build has.
    // 6.30 (CL-4573096) keeps the pickups' toss curve as a TSoftObjectPtr<UCurveVector> too (+0x1380).
    static inline Reflection::FMemberInfo Member_PickupSplineCurvePath{ &StaticReflectionOwner, TEXT("PickupSplineCurve"), sizeof(TAssetPtr<UCurveVector>), Reflection::EMemberKind::Value, nullptr, Reflection::SoftPathGrowth<TAssetPtr<UCurveVector>> };
    static inline Reflection::FMemberInfo Member_LootQuotaDataPath{ &StaticReflectionOwner, TEXT("LootQuotaData"), sizeof(TAssetPtr<UDataTable>), Reflection::EMemberKind::Value, nullptr, Reflection::SoftPathGrowth<TAssetPtr<UDataTable>> };
    static inline Reflection::FMemberInfo Member_StoneItemDefinitionPath{ &StaticReflectionOwner, TEXT("StoneItemDefinition"), sizeof(TAssetPtr<UFortWorldItemDefinition>), Reflection::EMemberKind::Value, nullptr, Reflection::SoftPathGrowth<TAssetPtr<UFortWorldItemDefinition>> };
    static inline Reflection::FMemberInfo Member_WoodItemDefinitionPath{ &StaticReflectionOwner, TEXT("WoodItemDefinition"), sizeof(TAssetPtr<UFortWorldItemDefinition>), Reflection::EMemberKind::Value, nullptr, Reflection::SoftPathGrowth<TAssetPtr<UFortWorldItemDefinition>> };
    static inline Reflection::FMemberInfo Member_MetalItemDefinitionPath{ &StaticReflectionOwner, TEXT("MetalItemDefinition"), sizeof(TAssetPtr<UFortWorldItemDefinition>), Reflection::EMemberKind::Value, nullptr, Reflection::SoftPathGrowth<TAssetPtr<UFortWorldItemDefinition>> };
    static inline Reflection::FMemberInfo Member_PermaniteItemDefinitionPath{ &StaticReflectionOwner, TEXT("PermaniteItemDefinition"), sizeof(TAssetPtr<UFortWorldItemDefinition>), Reflection::EMemberKind::Value, nullptr, Reflection::SoftPathGrowth<TAssetPtr<UFortWorldItemDefinition>> };
    static inline Reflection::FMemberInfo Member_BluGloItemDefinitionPath{ &StaticReflectionOwner, TEXT("BluGloItemDefinition"), sizeof(TAssetPtr<UFortWorldItemDefinition>), Reflection::EMemberKind::Value, nullptr, Reflection::SoftPathGrowth<TAssetPtr<UFortWorldItemDefinition>> };

    UFortWorldItemDefinition* GetItemDefinitionMember(Reflection::FMemberInfo& SoftMember, Reflection::FMemberInfo& ObjectMember) const;

public:
    // The game data's resource and BluGlo items, loading them from their paths on 6.30+.
    UFortWorldItemDefinition* GetStoneItemDefinition() const { return GetItemDefinitionMember(Member_StoneItemDefinitionPath, Member_StoneItemDefinition); }
    UFortWorldItemDefinition* GetWoodItemDefinition() const { return GetItemDefinitionMember(Member_WoodItemDefinitionPath, Member_WoodItemDefinition); }
    UFortWorldItemDefinition* GetMetalItemDefinition() const { return GetItemDefinitionMember(Member_MetalItemDefinitionPath, Member_MetalItemDefinition); }
    UFortWorldItemDefinition* GetPermaniteItemDefinition() const { return GetItemDefinitionMember(Member_PermaniteItemDefinitionPath, Member_PermaniteItemDefinition); }
    UFortWorldItemDefinition* GetBluGloItemDefinition() const { return GetItemDefinitionMember(Member_BluGloItemDefinitionPath, Member_BluGloItemDefinition); }
    // The curve a tossed pickup flies along (AFortPickup::GetRandomFlyTime), loaded from its path on 6.30+.
    UCurveVector* GetPickupSplineCurve() const;
    // LootQuotaData as the game data holds it (loaded from its path on 6.30+), without GetLootQuotaData's fallbacks.
    UDataTable* GetLootQuotaDataMember() const;
    // Points LootQuotaData at Table (on 6.30+ at its path; the table must stay loaded, as Core's rooted ones do).
    void SetLootQuotaDataMember(UDataTable* Table);

    std::vector<UDataTable*> GetLootTierDataTables();
    std::vector<UDataTable*> GetLootPackageDataTables();
    bool HasLootTierDataTables();
    bool HasLootPackageDataTables();
    UDataTable* GetLootQuotaData();
    FFortLootTierData* FindLootTierDataRow(FName LootTierKey, const wchar_t* ContextString);

    static ptrdiff_t ConditionalFoundationQuotaOffset() { return Member_ConditionalFoundationQuota.GetOffsetChecked(); }
    static bool HasLootTierDataTablesMember() { return Member_LootTierDataTables.IsAvailable(); }
    static ptrdiff_t LootTierDataTablesOffset() { return Member_LootTierDataTables.GetOffsetChecked(); }

    const FQuickBarData& GetQuickBarDefinition(EFortQuickBars InQuickBar) const
    {
        const FQuickBarData& Definitions = QuickBarDefinitions;
        const uint8_t* Definition = reinterpret_cast<const uint8_t*>(&Definitions) + static_cast<size_t>(InQuickBar) * FQuickBarData::StaticStruct()->GetStructureSize();
        return *reinterpret_cast<const FQuickBarData*>(Definition);
    }
};
