#pragma once

#include "GameFramework/Actor.h"

// Battle Royale's storm circle (from 1.7.2): the game mode moves it phase by phase, shrinking Radius from LastRadius to
// NextRadius between SafeZoneStartShrinkTime and SafeZoneFinishShrinkTime.
class AFortSafeZoneIndicator : public AActor
{
    DefineUnrealClass(AFortSafeZoneIndicator, FortniteGame)

public:
    DefineUProperty(float, Radius)
    DefineUProperty(float, LastRadius)
    DefineUProperty(float, NextRadius)
};
