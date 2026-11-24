#pragma once

#include "UObject/Object.h"

// The perception of Battle Royale's AI controllers (AAthenaAIController's PerceptionComponent subobject): the six senses
// UFortAIPerceptionComponent's constructor makes (sight, hearing, damage, touch, team, prediction), less sight and
// prediction where two statics of the class are off.
class UAthenaAIPerceptionComponent : public UObject
{
    DefineUnrealClass(UAthenaAIPerceptionComponent, FortniteGame)

public:
    // Turns on the two statics the constructor keeps sight and prediction by, before any AI controller is made.
    static void EnableServerSenses();
};
