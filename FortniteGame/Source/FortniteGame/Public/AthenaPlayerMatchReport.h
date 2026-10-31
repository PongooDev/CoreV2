#pragma once

#include "UObject/Object.h"

// A Battle Royale player's stats for one match (from 1.10; FortniteGame/Public/AthenaPlayerMatchReport.h), which the
// end-of-match screen shows (AthenaMatchStatsPartial: placement, Kills, Assists, Revives, Accuracy, Hits, Critical
// Hits, Distance Traveled, Materials Gathered and Used, Damage To Players, Damage Taken and To Structures). 1.10 names
// each stat; later builds (21.40) keep them in an int32 Stats[20] beside a StatBucket and the match's id. Members a
// build doesn't reflect are left out (IsAvailable).
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
    DefineUProperty(int32_t, RangedHit)
    DefineUProperty(int32_t, RangedMiss)
    // RangedHit over the shots fired, 0 to 1: the screen's AccuracyStat is a Percentage.
    DefineUProperty(float, Accuracy)
    // In centimetres: the screen's DistanceStat is a Distance, which shows them as metres or kilometres.
    DefineUProperty(int32_t, TravelDistanceGround)
    DefineUProperty(int32_t, MaterialsGathered)
    DefineUProperty(int32_t, MaterialsUsed)
    DefineUProperty(int32_t, CriticalShots)

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
    static Reflection::FMemberInfo& StatRangedHit() { return Member_RangedHit; }
    static Reflection::FMemberInfo& StatRangedMiss() { return Member_RangedMiss; }
    static Reflection::FMemberInfo& StatAccuracy() { return Member_Accuracy; }
    static Reflection::FMemberInfo& StatTravelDistanceGround() { return Member_TravelDistanceGround; }
    static Reflection::FMemberInfo& StatMaterialsGathered() { return Member_MaterialsGathered; }
    static Reflection::FMemberInfo& StatMaterialsUsed() { return Member_MaterialsUsed; }
    static Reflection::FMemberInfo& StatCriticalShots() { return Member_CriticalShots; }
};

// The rewards of a match (FAthenaAwardGroups of MCP loot, the season XP among them), which MCP's EndBattleRoyaleGame
// answered with; the end-of-match screen's "Team Still Alive" wait ends when they come.
struct FAthenaRewardResult
{
    DefineUnrealStruct(FAthenaRewardResult, FortniteGame)
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

    static bool HasStats() { return Member_bHasStats.IsAvailable() && Member_Stats.IsAvailable(); }
};
