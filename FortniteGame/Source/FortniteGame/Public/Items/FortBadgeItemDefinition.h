#pragma once

#include "Items/FortItemDefinition.h"

class UFortBadgeItemDefinition : public UFortWorldItemDefinition
{
    DefineUnrealClass(UFortBadgeItemDefinition, FortniteGame)
    DefineUFunction(GetMissionPoints)

public:
    int32_t GetMissionPoints();
};
