#pragma once

#include "Items/FortItemDefinition.h"

class UFortSchematicItemDefinition : public UFortItemDefinition
{
    DefineUnrealClass(UFortSchematicItemDefinition, FortniteGame)
    DefineUFunction(GetResultWorldItemDefinition)

public:
    UFortWorldItemDefinition* GetResultWorldItemDefinition() const;
};
