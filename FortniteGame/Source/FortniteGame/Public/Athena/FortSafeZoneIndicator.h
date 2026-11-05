#pragma once

#include "GameFramework/Actor.h"

// Battle Royale's storm circle (from 1.7.2): the game mode moves it phase by phase, shrinking Radius from LastRadius to
// NextRadius between SafeZoneStartShrinkTime and SafeZoneFinishShrinkTime.
class AFortSafeZoneIndicator : public AActor
{
    DefineUnrealClass(AFortSafeZoneIndicator, FortniteGame)
    DefineUFunction(GetSafeZoneCenter)

public:
    DefineUProperty(float, Radius)
    DefineUProperty(float, LastRadius)
    DefineUProperty(float, NextRadius)
    // The next circle's centre (an FVector_NetQuantize100, an FVector).
    DefineUProperty(FVector, NextCenter)
    static bool HasNextCenter() { return Member_NextCenter.IsAvailable(); }
    // When the circle starts and finishes shrinking to NextRadius (the server's world time, replicated).
    DefineUProperty(float, SafeZoneStartShrinkTime)
    DefineUProperty(float, SafeZoneFinishShrinkTime)
    static bool HasShrinkTimes() { return Member_SafeZoneStartShrinkTime.IsAvailable() && Member_SafeZoneFinishShrinkTime.IsAvailable(); }
    // The storm circle's centre now (it moves while the circle shrinks); Radius is its radius now.
    FVector GetSafeZoneCenter() const;
};
