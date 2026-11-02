#pragma once

#include "StructArray.h"
#include "UObject/Object.h"

// A Battle Royale player's stats for one match (from 1.10; FortniteGame/Public/AthenaPlayerMatchReport.h), which the
// end-of-match screen shows (AthenaMatchStatsPartial: placement, Kills, Assists, Revives, Accuracy, Hits, Critical
// Hits, Distance Traveled, Materials Gathered and Used, Damage To Players, Damage Taken and To Structures). 1.10 names
// each stat; later builds (21.40) keep them in an int32 Stats[20] beside a StatBucket and the match's id. 1.11 moves
// Place and TotalPlayers out to FAthenaMatchTeamStats, has RangedShots for RangedMiss, splits the materials by type
// and the damage and hits into normal and critical ones (its screen's Headshots is CriticalHitsToHostiles, where
// 1.10's is CriticalShots). Members a build doesn't reflect are left out (IsAvailable).
struct FAthenaMatchStats
{
    DefineUnrealStruct(FAthenaMatchStats, FortniteGame)
    DefineUProperty(int32_t, Place)
    DefineUProperty(int32_t, TotalPlayers)
    DefineUProperty(int32_t, SecondsAlive)
    DefineUProperty(int32_t, Kills)
    DefineUProperty(int32_t, Downs)
    DefineUProperty(int32_t, Assists)
    DefineUProperty(int32_t, Revives)
    DefineUProperty(int32_t, DamageDealtToHostiles)
    DefineUProperty(int32_t, DamageDealtToFriends)
    DefineUProperty(int32_t, DamageDealtToStructures)
    DefineUProperty(int32_t, DamageTaken)
    // 1.11: the shots fired.
    DefineUProperty(int32_t, RangedShots)
    DefineUProperty(int32_t, RangedHit)
    DefineUProperty(int32_t, RangedMiss)
    // RangedHit over the shots fired, 0 to 1: the screen's AccuracyStat is a Percentage.
    DefineUProperty(float, Accuracy)
    // In centimetres: the screen's DistanceStat is a Distance, which shows them as metres or kilometres.
    DefineUProperty(int32_t, TravelDistanceGround)
    DefineUProperty(int32_t, GatheredWood)
    DefineUProperty(int32_t, GatheredStone)
    DefineUProperty(int32_t, GatheredMetal)
    DefineUProperty(int32_t, MaterialsGathered)
    DefineUProperty(int32_t, UsedWood)
    DefineUProperty(int32_t, UsedStone)
    DefineUProperty(int32_t, UsedMetal)
    DefineUProperty(int32_t, MaterialsUsed)
    DefineUProperty(int32_t, CriticalShots)
    DefineUProperty(int32_t, CriticalHitsToHostiles)

public:
    static bool IsReflected() { return StaticStruct() != nullptr; }

    // Sets a stat the build reflects; a build without it keeps its struct as it is.
    template <typename T>
    void SetStat(Reflection::FMemberInfo& Member, T Value)
    {
        if (Member.IsAvailable())
        {
            Reflection::AccessMember<T>(this, Member) = Value;
        }
    }

    static Reflection::FMemberInfo& StatPlace() { return Member_Place; }
    static Reflection::FMemberInfo& StatTotalPlayers() { return Member_TotalPlayers; }
    static Reflection::FMemberInfo& StatSecondsAlive() { return Member_SecondsAlive; }
    static Reflection::FMemberInfo& StatKills() { return Member_Kills; }
    static Reflection::FMemberInfo& StatDowns() { return Member_Downs; }
    static Reflection::FMemberInfo& StatAssists() { return Member_Assists; }
    static Reflection::FMemberInfo& StatRevives() { return Member_Revives; }
    static Reflection::FMemberInfo& StatDamageDealtToHostiles() { return Member_DamageDealtToHostiles; }
    static Reflection::FMemberInfo& StatDamageDealtToFriends() { return Member_DamageDealtToFriends; }
    static Reflection::FMemberInfo& StatDamageDealtToStructures() { return Member_DamageDealtToStructures; }
    static Reflection::FMemberInfo& StatDamageTaken() { return Member_DamageTaken; }
    static Reflection::FMemberInfo& StatRangedShots() { return Member_RangedShots; }
    static Reflection::FMemberInfo& StatRangedHit() { return Member_RangedHit; }
    static Reflection::FMemberInfo& StatRangedMiss() { return Member_RangedMiss; }
    static Reflection::FMemberInfo& StatAccuracy() { return Member_Accuracy; }
    static Reflection::FMemberInfo& StatTravelDistanceGround() { return Member_TravelDistanceGround; }
    static Reflection::FMemberInfo& StatGatheredWood() { return Member_GatheredWood; }
    static Reflection::FMemberInfo& StatGatheredStone() { return Member_GatheredStone; }
    static Reflection::FMemberInfo& StatGatheredMetal() { return Member_GatheredMetal; }
    static Reflection::FMemberInfo& StatMaterialsGathered() { return Member_MaterialsGathered; }
    static Reflection::FMemberInfo& StatUsedWood() { return Member_UsedWood; }
    static Reflection::FMemberInfo& StatUsedStone() { return Member_UsedStone; }
    static Reflection::FMemberInfo& StatUsedMetal() { return Member_UsedMetal; }
    static Reflection::FMemberInfo& StatMaterialsUsed() { return Member_MaterialsUsed; }
    static Reflection::FMemberInfo& StatCriticalShots() { return Member_CriticalShots; }
    static Reflection::FMemberInfo& StatCriticalHitsToHostiles() { return Member_CriticalHitsToHostiles; }
};

// The place of the player's team and how many played (from 1.11, which took them out of FAthenaMatchStats). The
// screen's OnTeamStatsAvailable writes them as "You came in {Place} out of {Players} Players"; its SetMatchReport binds
// it to the report's OnRewardsAvailable, so the screen writes them when the rewards come.
struct FAthenaMatchTeamStats
{
    DefineUnrealStruct(FAthenaMatchTeamStats, FortniteGame)
    DefineUProperty(int32_t, Place)
    DefineUProperty(int32_t, TotalPlayers)

public:
    static bool IsReflected() { return StaticStruct() != nullptr; }
};

// A match XP boost (from 1.11): its EAthenaMatchXpMultiplierSource (1 the Battle Pass's "Battle Pass Bonus", 2 Friend,
// 3 Gear) and its percent.
struct FAthenaMatchXpMultiplierGroup
{
    DefineUnrealStruct(FAthenaMatchXpMultiplierGroup, FortniteGame)
    DefineUProperty(uint8_t, Source)
    DefineUProperty(int32_t, Amount)
};

// What one source gave the player (ERewardSource: 1 MinutesPlayed "Survival Time", 3 TeamKills "Kills", 4 Placement,
// ...), with its MCP loot. From 1.11 also its Score, SeasonXp and BookXp; 1.10's has the source and loot only.
struct FAthenaAwardGroup
{
    DefineUnrealStruct(FAthenaAwardGroup, FortniteGame)
    DefineUProperty(uint8_t, RewardSource)
    DefineUProperty(int32_t, Score)
    DefineUProperty(float, SeasonXp)
    DefineUProperty(int32_t, BookXp)

public:
    static bool HasScore() { return Member_Score.IsAvailable() && Member_SeasonXp.IsAvailable(); }
    static bool HasBookXp() { return Member_BookXp.IsAvailable(); }
};

// The rewards of a match, which MCP's EndBattleRoyaleGame answered with (UFortMcpProfileAthena::EndBattleRoyaleGame);
// the end-of-match screen's "Team Still Alive" wait ends when they come. 1.11's (2.1 sub_45E100 copies it into the
// client's report, EndOfMatchResults): the levels and Battle Pass tiers gained, the season XP and Battle Stars, the XP
// boosts and the award groups. The screen's XP bar (XpMatchProgression) plays each group with a Score or SeasonXp as
// "<source> +<Score>" (2.1 sub_4759C0), then shares the rest of TotalSeasonXpGained among the boosts by their percent;
// its Total Score is the groups' Scores added up (sub_4748B0), its Total XP TotalSeasonXpGained (sub_474900). 1.10's
// has LevelsGained and the groups only. Members a build doesn't reflect are left out.
struct FAthenaRewardResult
{
    DefineUnrealStruct(FAthenaRewardResult, FortniteGame)
    DefineUProperty(int32_t, LevelsGained)
    DefineUProperty(int32_t, BookLevelsGained)
    DefineUProperty(int32_t, TotalSeasonXpGained)
    DefineUProperty(int32_t, TotalBookXpGained)
    DefineUStructArrayProperty(FAthenaMatchXpMultiplierGroup, XpMultipliers)
    DefineUStructArrayProperty(FAthenaAwardGroup, Rewards)

public:
    static bool IsReflected() { return StaticStruct() != nullptr; }

    // Sets a member the build reflects; a build without it keeps its struct as it is.
    template <typename T>
    void SetReward(Reflection::FMemberInfo& Member, T Value)
    {
        if (Member.IsAvailable())
        {
            Reflection::AccessMember<T>(this, Member) = Value;
        }
    }

    static Reflection::FMemberInfo& RewardLevelsGained() { return Member_LevelsGained; }
    static Reflection::FMemberInfo& RewardBookLevelsGained() { return Member_BookLevelsGained; }
    static Reflection::FMemberInfo& RewardTotalSeasonXpGained() { return Member_TotalSeasonXpGained; }
    static Reflection::FMemberInfo& RewardTotalBookXpGained() { return Member_TotalBookXpGained; }
    static bool HasXpMultipliers() { return Member_XpMultipliers.IsAvailable() && FAthenaMatchXpMultiplierGroup::StaticStruct() != nullptr; }
    static bool HasRewards() { return Member_Rewards.IsAvailable() && FAthenaAwardGroup::StaticStruct() != nullptr; }
};

// A player's report of their match (from 1.10; AFortPlayerControllerAthena::MatchReport, made with the controller in
// its BeginPlay, 1.10 0x42B1B0, on the server as on the client). The client's fills from the server's RPCs
// (ClientSendMatchStatsForPlayer 0x42F030, ClientSendEndBattleRoyaleMatchForPlayer 0x42EFC0), which set the struct,
// its bHas flag and broadcast OnStatsAvailable / OnRewardsAvailable to the screen. The server's holds what the server
// sent, so bHasStats on it says the player's stats are out (21.40's controller adds bMatchStatsForPlayerSent for that).
class UAthenaPlayerMatchReport : public UObject
{
    DefineUnrealClass(UAthenaPlayerMatchReport, FortniteGame)

public:
    DefineUProperty(bool, bHasStats)
    DefineUStructProperty(FAthenaMatchStats, Stats)
    // 1.11 (ClientSendTeamStatsForPlayer).
    DefineUProperty(bool, bHasTeamStats)
    DefineUStructProperty(FAthenaMatchTeamStats, TeamStats)

    static bool HasStats() { return Member_bHasStats.IsAvailable() && Member_Stats.IsAvailable(); }
    static bool HasTeamStats() { return FAthenaMatchTeamStats::IsReflected() && Member_bHasTeamStats.IsAvailable() && Member_TeamStats.IsAvailable(); }
};
