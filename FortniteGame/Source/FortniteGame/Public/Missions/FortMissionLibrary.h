#pragma once

#include "UObject/NameTypes.h"
#include "UObject/Object.h"

class UFortMissionLibrary : public UObject
{
    DefineUnrealClass(UFortMissionLibrary, FortniteGame)
    DefineUFunction(ExecuteMissionEvent)

public:
    static void ExecuteMissionEvent(UObject* WorldContextObject, FName EventType, UObject* Params);
};
