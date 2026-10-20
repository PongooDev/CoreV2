#pragma once

#include "GameplayTagContainer.h"
#include "Items/FortQuickBars.h"
#include "StructArray.h"
#include "UObject/AssetPtr.h"
#include "UObject/Object.h"

class AActor;
class AFortAIDirector;
class UEnvQuery;
class UFortAbilitySet;
class UFortItemDefinition;

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

class UFortGameData : public UObject
{
    DefineUnrealClass(UFortGameData, FortniteGame)

public:
    DefineUStructProperty(FQuickBarData, QuickBarDefinitions)
    DefineUStructProperty(FFortPlayerSpawnPadPlacementData, DefaultPlayerSpawnPadPlacementData)
    DefineUProperty(TAssetPtr<UFortAbilitySet>, GenericPlayerAbilitySet)
    DefineUStructArrayProperty(FItemDefinitionAndCount, DefaultInventoryList)
    DefineUProperty(TArray<TAssetSubclassOf<AFortAIDirector>>, AIDirectors)

    const FQuickBarData& GetQuickBarDefinition(EFortQuickBars InQuickBar) const
    {
        const FQuickBarData& Definitions = QuickBarDefinitions;
        const uint8_t* Definition = reinterpret_cast<const uint8_t*>(&Definitions) + static_cast<size_t>(InQuickBar) * FQuickBarData::StaticStruct()->GetStructureSize();
        return *reinterpret_cast<const FQuickBarData*>(Definition);
    }
};
