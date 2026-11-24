#pragma once

#include "GameFramework/Info.h"
#include "Reflection.h"

class AAthenaFillFloor;

// The rising floor's mutator (from 8.40; Food Fight: Deep Fried's Mutator_Lava_C). All of it is in 8.40 (CL-6005771):
// OnPlaylistLoaded (0xFE9F00) spawns FloorActorClass as LavaFloor, OnGamePhaseChanged (0xFE78E0) starts it moving.
// Core only reads its floor. Core declares no AFortGameplayMutator, so this derives from its AInfo.
class AFortAthenaMutator_Fill : public AInfo
{
    DefineUnrealClass(AFortAthenaMutator_Fill, FortniteGame)

public:
    DefineUProperty(AAthenaFillFloor*, LavaFloor)
};
