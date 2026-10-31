#pragma once

#include "Containers/Array.h"
#include "UObject/NameTypes.h"
#include "UObject/Object.h"

// How long a stat's value runs before it starts again: Minute, Life, Map (the match), Campaign, Persistent. A stat
// keeps one value for each period up to its HighestPeriodToTrack.
class EStatRecordingPeriod
{
    DefineUnrealEnum(EStatRecordingPeriod, FortniteGame)
    DefineUEnumValue(Minute)
    DefineUEnumValue(Life)
    DefineUEnumValue(Map)
};

// One of a player's stats: its name and, natively, a value per recording period (doubles).
class UStat : public UObject
{
    DefineUnrealClass(UStat, FortniteGame)

public:
    DefineUProperty(FName, StatName)
};

// A player's gameplay stats (AFortPlayerController::StatManager; 21.40's UBaseStatManager), kept by the game's own
// server code in 1.10 under names the screen's FAthenaMatchStats are made from: the pawn's damage handler (1.10 0x8374E0)
// adds the damage a player's weapon deals as "<source>.Damage.Dealt.ToHostile" or ".ToFriend" (its sources are the
// weapon's AnalyticTags, 0x8F1AF0 adding to each), and "DamageDealt"; the damage handler after it (0x838390) the damage
// a player takes as "DamageReceived"; a building's (0x480EC0) "<source>.Damage.Dealt.ToStructure"; a weapon's fire
// (0x2A1C70) "<source>.Ranged.Fire", its hit on another team's player (0x43D8E0) "<source>.Ranged.Hit.Hostile";
// a head shot (0x9B38A0) "Headshots"; the pawn's movement on the ground (0x454FF0) "Travel.Distance.Ground" in
// centimetres; and the inventory "WoodAcquired" ... "MetalExpended" (0x8A9F00, names 0x8EDD30). Their match values are
// the Map period's.
class UStatManager : public UObject
{
    DefineUnrealClass(UStatManager, FortniteGame)

public:
    DefineUProperty(TArray<UStat*>, Stats)

    // The stat's value in Period, rounded, 0 when the player has no such stat (1.10 0x9006E0).
    int32_t GetStatValue(FName StatName, EStatRecordingPeriod Period);
    // The sum of "<source>.<Stat>" over every source the player has the stat for: what ModifyStatForAllSources
    // (0x8F1AF0) added to.
    int32_t GetStatValueForAllSources(const wchar_t* Stat, EStatRecordingPeriod Period);
};
