#pragma once

#include "ObjectLayout.h"
#include "UObject/Object.h"

// Implemented by missions, objectives and the mission props a blueprint sends mission events from (Wounded Survivor's
// B_MEDBot, implemented in the blueprint).
class UFortMissionInterface : public UObject
{
    DefineUnrealClass(UFortMissionInterface, FortniteGame)
};
