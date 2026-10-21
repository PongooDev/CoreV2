#pragma once

#include "GameplayTagContainer.h"
#include "Misc/Guid.h"
#include "UObject/NameTypes.h"
#include "UObject/Object.h"

class UFortMissionLibrary : public UObject
{
    DefineUnrealClass(UFortMissionLibrary, FortniteGame)
    DefineUFunction(ExecuteMissionEvent)

public:
    static void ExecuteMissionEvent(UObject* WorldContextObject, FName EventType, UObject* Params);
    static void ExecuteMissionEvent(UObject* WorldContextObject, FName EventType, const FGuid& MissionGuid, const FGameplayTagContainer& ObjectiveHandle, UObject* Params);
};
