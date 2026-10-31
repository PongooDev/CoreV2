#pragma once

#include "Containers/Set.h"
#include "FortTeamInfo.h"
#include "Player/FortPlayerState.h"

class AFortPlayerStateAthena;

// The banner a Battle Royale player shows in the match (from 1.10: the elimination screen's), their loadout's.
struct FPlayerBannerInfo
{
    DefineUnrealStruct(FPlayerBannerInfo, FortniteGame)
    DefineUProperty(FString, IconId)
    DefineUProperty(FString, ColorId)
    DefineUProperty(int32_t, Level)
};

// How a Battle Royale player was last downed or eliminated, which the death screen shows (OnRep_DeathInfo). The death
// handler (1.7.2 0x437D2F, 1.8 0x43BAFE) sets it when the player is eliminated: not DBNO, and the eliminator's player
// state, or the player's own when nobody eliminated them (the storm, a fall). Its first member is Killer up to 1.7.2,
// FinisherOrDowner from 1.8; only 1.8's is read (GetEliminator, for AFortGameModeAthena's results).
struct FDeathInfo
{
    DefineUnrealStruct(FDeathInfo, FortniteGame)
    DefineUProperty(AFortPlayerStateAthena*, FinisherOrDowner)
    DefineUProperty(bool, bDBNO)
    // EDeathCause (from 1.8): OutsideSafeZone, FallDamage, Pistol, ... Trap, DBNOTimeout, ..., Unspecified.
    DefineUProperty(uint8_t, DeathCause)

public:
    static bool IsReadable()
    {
        return Member_FinisherOrDowner.IsAvailable() && Member_bDBNO.IsAvailable();
    }

    static ptrdiff_t DeathCauseOffset() { return Member_DeathCause.GetOffsetChecked(); }
    static ptrdiff_t DBNOOffset() { return Member_bDBNO.GetOffsetChecked(); }
};

// Battle Royale's player state (from 1.7.2).
class AFortPlayerStateAthena : public AFortPlayerStateZone
{
    DefineUnrealClass(AFortPlayerStateAthena, FortniteGame)

public:
    // The player's team, copied from its AFortTeamInfo when it joins one (0x44AB80).
    DefineUProperty(EFortTeam, TeamIndex)
    // Set when the player's team is out of the match (the teams left, for each player of it: 0x44A870) or has won (1).
    DefineUProperty(int32_t, Place)
    // Other teams' players the player eliminated: Kills up to 1.7.2, KillScore from 1.8 (GetKills). 1.7.2's death
    // handler adds to Kills (0x437DB0); 1.8's adds to nothing (KillScore's only writer copies another player state's,
    // 0x4216C0), so Core counts 1.8's kills itself (CountsKills; AFortGameModeAthena's results).
    DefineUProperty(int32_t, Kills)
    DefineUProperty(int32_t, KillScore)
    DefineUStructProperty(FDeathInfo, DeathInfo)
    // 1.10: the other teams' players the player knocked down (the DBNO handling adds to it), and those they hit (the
    // hit's stat, 0x43D8E0, adds each to the set), for the end-of-match screen's Downs and Assists.
    DefineUProperty(int32_t, DownScore)
    static bool HasDownScore() { return Member_DownScore.IsAvailable(); }

private:
    // A set can't be assigned, so it has no DefineUProperty setter: read-only.
    static inline Reflection::FMemberInfo Member_HostilePlayersDamaged{ &StaticReflectionOwner, TEXT("HostilePlayersDamaged"), sizeof(TSet<AFortPlayerStateAthena*>), Reflection::EMemberKind::Value };

public:
    const TSet<AFortPlayerStateAthena*>& GetHostilePlayersDamaged() const { return Reflection::AccessMember<TSet<AFortPlayerStateAthena*>>(this, Member_HostilePlayersDamaged); }
    static bool HasHostilePlayersDamaged() { return Member_HostilePlayersDamaged.IsAvailable(); }
    // Set when the player is eliminated or wins.
    DefineUProperty(int32_t, SecondsAlive)
    // From 1.10, replicated to every client.
    DefineUStructProperty(FPlayerBannerInfo, Banner)
    DefineUFunction(ClientReportKill)

    // The kill feed line on the eliminator's client ("You eliminated <name>").
    void ClientReportKill(const FString& KilledPlayersName);

    static ptrdiff_t PlaceOffset() { return Member_Place.GetOffsetChecked(); }
    static ptrdiff_t SecondsAliveOffset() { return Member_SecondsAlive.GetOffsetChecked(); }
    static ptrdiff_t DeathInfoOffset() { return Member_DeathInfo.GetOffsetChecked(); }
    static bool HasBanner() { return Member_Banner.IsAvailable(); }

    // Client only, not replicated: whether the player is talking in voice chat, which the HUD's team list shows (its
    // speaker icon). Only Vivox's participant updates set it (1.7.2 0x3C54D20); Core sets it from the engine's voice chat.
    DefineUProperty(bool, bIsTalking)

    int32_t GetKills()
    {
        return Member_Kills.IsAvailable() ? Kills : Member_KillScore.IsAvailable() ? KillScore : 0;
    }

    // Whether the build's own death handler counts the players' kills (1.7.2's Kills), else Core sets KillScore.
    static bool CountsKills()
    {
        return Member_Kills.IsAvailable();
    }

    static bool HasEliminator()
    {
        return Member_DeathInfo.IsAvailable() && FDeathInfo::IsReadable();
    }

    // Who eliminated the player (DeathInfo, 1.8): nullptr while they're in the match or DBNO, themselves when nobody did.
    AFortPlayerStateAthena* GetEliminator()
    {
        if (!HasEliminator())
        {
            return nullptr;
        }
        FDeathInfo& Info = DeathInfo;
        return Info.bDBNO ? nullptr : Info.FinisherOrDowner;
    }

    void SetKills(int32_t Count)
    {
        if (Member_KillScore.IsAvailable() && KillScore != Count)
        {
            KillScore = Count;
        }
    }
};
