#pragma once

#include "GameplayTagContainer.h"
#include "UObject/AssetPtr.h"
#include "UObject/Object.h"

class AActor;
class UEnvQuery;

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
    DefineUStructProperty(FFortPlayerSpawnPadPlacementData, DefaultPlayerSpawnPadPlacementData)
};
