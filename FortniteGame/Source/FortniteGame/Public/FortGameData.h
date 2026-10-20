#pragma once

#include "GameplayTagContainer.h"
#include "Items/FortQuickBars.h"
#include "UObject/AssetPtr.h"
#include "UObject/Object.h"

class AActor;
class UEnvQuery;
class UFortAbilitySet;

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

    const FQuickBarData& GetQuickBarDefinition(EFortQuickBars InQuickBar) const
    {
        const FQuickBarData& Definitions = QuickBarDefinitions;
        const uint8_t* Definition = reinterpret_cast<const uint8_t*>(&Definitions) + static_cast<size_t>(InQuickBar) * FQuickBarData::StaticStruct()->GetStructureSize();
        return *reinterpret_cast<const FQuickBarData*>(Definition);
    }
};
