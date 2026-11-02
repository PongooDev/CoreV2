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

// How UStatManager::ModifyStat changes a stat's value in each period it tracks: adds the amount (Delta), sets it (Set)
// or keeps the larger of the two (Maximum).
class EStatMod
{
    DefineUnrealEnum(EStatMod, FortniteGame)
    DefineUEnumValue(Delta)
    DefineUEnumValue(Set)
    DefineUEnumValue(Maximum)
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
// adds the damage a player's weapon deals as "Damage.Dealt.ToHostile:<source>" or "Damage.Dealt.ToFriend:<source>"
// (its sources are the weapon's AnalyticTags, 0x8F1AF0 adding to each), and "DamageDealt"; the damage handler after it
// (0x838390) the damage a player takes as "DamageReceived"; a building's (0x480EC0) "Damage.Dealt.ToStructure:<source>";
// a weapon's fire (0x2A1C70) "Ranged.Fire:<source>" (1.11 on: "Ranged.Bullets.Fired:<source>", 2.1 0x2A35D0), its hit
// on another team's player (0x43D8E0) "Ranged.Hit.Hostile:<source>"; a head shot (0x9B38A0) "Headshots"; the pawn's
// movement on the ground (0x454FF0) "Travel.Distance.Ground" in centimetres; and the building costs paid (0x8A9F00)
// "WoodExpended" ... "MetalExpended". No code adds to "WoodAcquired" ... "MetalAcquired" (1.10 to 2.2): the materials
// gathered are the player state's WoodGathered ... MetalGathered (AFortPlayerState::GetStatValue). Their match values
// are the Map period's.
class UStatManager : public UObject
{
    DefineUnrealClass(UStatManager, FortniteGame)

public:
    DefineUProperty(TArray<UStat*>, Stats)

    // The player's stat of that name, nullptr when they have none: UStatManager::ModifyStat (2.1 0x91C510) changes only a
    // stat that exists ("Attempt to modify stat '%s' failed as it cannot be found!").
    UStat* FindStat(FName StatName);
    // The stat's value in Period, rounded, 0 when the player has no such stat (1.10 0x9006E0).
    int32_t GetStatValue(FName StatName, EStatRecordingPeriod Period);
    // The sum of "<Stat>:<source>" over every source the player has the stat for: what ModifyStatForAllSources
    // (0x8F1AF0) added to.
    int32_t GetStatValueForAllSources(const wchar_t* Stat, EStatRecordingPeriod Period);
};
