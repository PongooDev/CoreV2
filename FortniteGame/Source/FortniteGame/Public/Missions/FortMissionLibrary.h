#pragma once

#include "GameplayTagContainer.h"
#include "Misc/Guid.h"
#include "Math/Vector.h"
#include "UObject/NameTypes.h"
#include "UObject/Object.h"
#include "UObject/Stack.h"

class AFortPlayerController;

class UFortMissionLibrary : public UObject
{
    DefineUnrealClass(UFortMissionLibrary, FortniteGame)
    DefineUFunction(ExecuteMissionEvent)
    DefineUFunction(SpawnAndCollectPlayerPickups)

public:
    static void ExecuteMissionEvent(UObject* WorldContextObject, FName EventType, UObject* Params);
    static void ExecuteMissionEvent(UObject* WorldContextObject, FName EventType, const FGuid& MissionGuid, const FGameplayTagContainer& ObjectiveHandle, UObject* Params);
    static void SpawnAndCollectPlayerPickups(UObject* WorldContextObject, AFortPlayerController* PlayerController, const FVector& StartPosition, FName LootTierGroup, int32_t LootTier);

    DECLARE_FUNCTION(execSpawnAndCollectPlayerPickups);

    static void HookNatives();
};
