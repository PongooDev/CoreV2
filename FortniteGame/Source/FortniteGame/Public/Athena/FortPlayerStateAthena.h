#pragma once

#include "Containers/Set.h"
#include "Engine/NetSerialization.h"
#include "FortTeamInfo.h"
#include "Natives.h"
#include "StructArray.h"
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

// One of the player states watching a player (21.40's FFortSpectatorZoneItem).
struct FFortSpectatorAthenaItem : public FFastArraySerializerItem
{
    DefineUnrealStruct(FFortSpectatorAthenaItem, FortniteGame)
    DefineUProperty(AFortPlayerStateAthena*, PlayerState)
};

// A player state's Spectators (21.40's FFortSpectatorZoneArray), replicated as a fast array: on the watched player's
// the player states spectating them, whose number is the HUD's "N spectating" (GetSpectatorCount, 3.3 0x11449B0, is
// SpectatorArray.Num()); on a spectator's, itself and the others watching the same player (BeginSpectating).
struct FFortSpectatorAthenaArray : public FFastArraySerializer
{
    DefineUnrealStruct(FFortSpectatorAthenaArray, FortniteGame)
    DefineUStructArrayProperty(FFortSpectatorAthenaItem, SpectatorArray)

public:
    static bool IsWritable() { return Member_SpectatorArray.IsAvailable() && Natives::FFastArraySerializer_MarkArrayDirty; }

    // 3.3 0xA10BF0 (21.40's AFortPlayerStateZone::AddSpectatingPlayer): a new item, unnumbered, and the array dirty.
    void AddSpectatingPlayer(AFortPlayerStateAthena* Spectator);
    // 3.3 0xA2D510 (21.40's AFortPlayerStateZone::RemoveSpectatingPlayer).
    void RemoveSpectatingPlayer(AFortPlayerStateAthena* Spectator);
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
    // The kills of the player's whole team (not replicated): 21.40's AFortPlayerStateAthena::SetKillScore keeps it for
    // each of the team's players.
    DefineUProperty(int32_t, TeamKillScore)
    DefineUStructProperty(FDeathInfo, DeathInfo)
    // 1.11: the player's squad within their team, 0 for none (OnRep_SquadId adds the squad's player states to the
    // owning controller's SquadMembers). AFortGameModeAthena::PlacePlayerOnTeam (2.1 0x444780) sets it from the
    // dedicated session's squads, which the match assignment's teams[i] made squad i + 1 (0x738440).
    DefineUProperty(uint8_t, SquadId)
    static bool HasSquadId() { return Member_SquadId.IsAvailable(); }
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
    // From 2.5 (Season 2's spectator count): the player state this one spectates (OnRep_SpectatingTarget) and those
    // spectating it (Spectators). The server's AFortPlayerControllerAthena::SpectateOnDeath (3.3 0xA31650) sets them
    // through BeginSpectating when bEnableSpectatorUpdates is on.
    DefineUProperty(AFortPlayerStateAthena*, SpectatingTarget)
    DefineUStructProperty(FFortSpectatorAthenaArray, Spectators)
    static bool HasSpectators() { return Member_SpectatingTarget.IsAvailable() && Member_Spectators.IsAvailable() && FFortSpectatorAthenaArray::IsWritable(); }

    // 3.3 0xA12BB0 (21.40's AFortPlayerStateZone::BeginSpectating): ends the current spectating, then watches
    // PlayerToSpectate: in its Spectators, and in those of the others watching it, who are added to this one's, with
    // itself. Not itself, nor the one it already watches.
    void BeginSpectating(AFortPlayerStateAthena* PlayerToSpectate);
    // 3.3 0xA19660: out of the watched player's Spectators and their spectators', SpectatingTarget cleared, and its own
    // Spectators emptied.
    void EndSpectating();

    // The kill feed line on the eliminator's client ("You eliminated <name>"): the killed player's name up to 1.9.1
    // (KilledPlayersName), their player state from 1.10 (Player; 1.11's client also counts the kill for the replay
    // highlights and broadcasts it to the HUD, 0x460180).
    void ClientReportKill(AFortPlayerStateAthena* KilledPlayer);

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

    static bool HasKillScore() { return Member_KillScore.IsAvailable(); }
    static bool HasTeamKillScore() { return Member_TeamKillScore.IsAvailable(); }

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

    // 21.40's SetKillScore: the player's KillScore, and TeamKillScore for every player of their team (the sum of the
    // team's KillScore). Replicated at once (ForceNetUpdate), so the HUD's kill count (OnRep_Kills) follows. Builds with
    // Kills (1.7.2) count their own: nothing is set there.
    void SetKillScore(int32_t NewKillScore);
};
