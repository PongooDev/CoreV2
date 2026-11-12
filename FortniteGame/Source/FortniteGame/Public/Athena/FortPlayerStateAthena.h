#pragma once

#include "Containers/Set.h"
#include "Engine/NetSerialization.h"
#include "FortTeamInfo.h"
#include "Math/Vector.h"
#include "Math/Vector2D.h"
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

// What killed a Battle Royale player (FDeathInfo's DeathCause, from 1.8).
class EDeathCause
{
    DefineUnrealEnum(EDeathCause, FortniteGame)
    DefineUEnumValue(OutsideSafeZone)
    DefineUEnumValue(Unspecified)
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
    // How far the eliminator's pawn was from the player, in cm (AActor::GetDistanceTo), or for a fall how far the player
    // fell (AFortPlayerPawnAthena::LastFallDistance, from 4.0), else 0. The client's OnRep_DeathInfo (3.5.1 0xA617C0)
    // shows it in metres (Distance * 0.01) after a Sniper kill feed line or past the game data's distance; 5.10's
    // (0xC492D0) always does after a fall.
    DefineUProperty(float, Distance)
    // Where the player died (from 3.5). AFortPlayerControllerAthena::RespawnPlayerAfterDeath (4.2 0xA57AD0) clears the
    // death info, this with it, once the player is back.
    DefineUProperty(FVector, DeathLocation)
    // Set once the death info holds a death (from 4.4). The player state's IsDead (4.5 0xAE25A0) is bInitialized &&
    // !bDBNO && (FinisherOrDowner || DeathCause != Unspecified): the squad bar's dead state (UAthenaPlayerInfoBase::
    // SetPlayerState, 0x2997B00, DeadStateChanged), the squad's count of teammates standing (0xA99E30) and their map
    // markers read it. The game's death handling sets it as it stores a death (0xAE1C40, AFortPlayerStateAthena::
    // SetDeathInfo); a death report written field by field doesn't.
    DefineUProperty(bool, bInitialized)
    // Flags of how the player died (from 4.5; not a UENUM): what the pawn's vehicle and tags said as the death was
    // stored (Natives::AFortPlayerPawnAthena_GetDeathCircumstance). The client's kill feed reads it for a fall.
    DefineUProperty(int32_t, DeathCircumstance)

public:
    static bool HasDistance() { return Member_Distance.IsAvailable(); }
    static bool HasDeathLocation() { return Member_DeathLocation.IsAvailable(); }
    static bool HasInitialized() { return Member_bInitialized.IsAvailable(); }
    static bool HasDeathCircumstance() { return Member_DeathCircumstance.IsAvailable(); }
    static bool IsReadable()
    {
        return Member_FinisherOrDowner.IsAvailable() && Member_bDBNO.IsAvailable();
    }

    static ptrdiff_t DeathCauseOffset() { return Member_DeathCause.GetOffsetChecked(); }
    static ptrdiff_t DBNOOffset() { return Member_bDBNO.GetOffsetChecked(); }
    static ptrdiff_t InitializedOffset() { return HasInitialized() ? Member_bInitialized.GetOffsetChecked() : -1; }
    static ptrdiff_t DeathCircumstanceOffset() { return HasDeathCircumstance() ? Member_DeathCircumstance.GetOffsetChecked() : -1; }
};

// Where a player who died comes back, and whether both sides are ready for it (from 6.02; not replicated). The pawn's
// death handler works the place out as it starts the respawn (6.02 0xC48A10: the game mode's respawn location, a
// mutator's first) and stores it with AFortPlayerControllerAthena::ServerSetRespawnData (0xC24EE0). The client is ready
// at once when the place is within the controller's RespawnLevelStreamDistance of the death and the playlist doesn't
// force the camera fade; else PrepareClientForRespawning sends it ClientStartRespawnPreparation and its
// ServerClientIsReadyToRespawn says so. RespawnPlayerAfterDeath (0xC23990) sets bServerIsReady and waits for both, and
// the game mode's FinishRestartPlayer (0xBC7430) moves the new pawn there and clears it, for a death that is stored
// (DeathInfo.bInitialized). All of it is the build's own; Core only reads it for its log.
struct FFortRespawnData
{
    DefineUnrealStruct(FFortRespawnData, FortniteGame)
    DefineUProperty(bool, bRespawnDataAvailable)
    DefineUProperty(bool, bClientIsReady)
    DefineUProperty(bool, bServerIsReady)
    DefineUProperty(FVector, RespawnLocation)

public:
    static bool IsReadable()
    {
        return Member_bRespawnDataAvailable.IsAvailable() && Member_bClientIsReady.IsAvailable() && Member_bServerIsReady.IsAvailable() && Member_RespawnLocation.IsAvailable();
    }
};

// One of the player states watching a player. 6.10 (CL-4480234) moved spectating from the Athena classes to their Zone
// parents and renamed the two structs with it: FFortSpectatorAthenaItem and FFortSpectatorAthenaArray up to 6.02.
struct FFortSpectatorZoneItem : public FFastArraySerializerItem
{
    DefineRenamedUnrealStruct(FFortSpectatorZoneItem, FFortSpectatorAthenaItem, FortniteGame)
    DefineUProperty(AFortPlayerStateAthena*, PlayerState)
};

// A player state's Spectators, replicated as a fast array: on the watched player's
// the player states spectating them, whose number is the HUD's "N spectating" (GetSpectatorCount, 3.3 0x11449B0, is
// SpectatorArray.Num()); on a spectator's, itself and the others watching the same player (BeginSpectating).
struct FFortSpectatorZoneArray : public FFastArraySerializer
{
    DefineRenamedUnrealStruct(FFortSpectatorZoneArray, FFortSpectatorAthenaArray, FortniteGame)
    DefineUStructArrayProperty(FFortSpectatorZoneItem, SpectatorArray)

public:
    static bool IsWritable() { return Member_SpectatorArray.IsAvailable() && (Natives::FFastArraySerializer_MarkArrayDirty || Natives::FFastArraySerializer_ItemMapReset); }

    // 3.3 0xA10BF0 (21.40's AFortPlayerStateZone::AddSpectatingPlayer): a new item, unnumbered, and the array dirty.
    void AddSpectatingPlayer(AFortPlayerStateAthena* Spectator);
    // 3.3 0xA2D510 (21.40's AFortPlayerStateZone::RemoveSpectatingPlayer).
    void RemoveSpectatingPlayer(AFortPlayerStateAthena* Spectator);
};

// What a squad's HUD shows over a player (from 3.5): a Squad Comms callout (NeedAmmoHeavy to NeedWeapon), or None.
class ETeamMemberState
{
    DefineUnrealEnum(ETeamMemberState, FortniteGame)
    DefineUEnumValue(None)
};

// Battle Royale's player state (from 1.7.2).
class AFortPlayerStateAthena : public AFortPlayerStateZone
{
    DefineUnrealClass(AFortPlayerStateAthena, FortniteGame)

public:
    // 3.5: the player's Squad Comms callout, replicated to everyone. OnRep_ReplicatedTeamMemberState (3.5.1 0xA66160)
    // copies a callout to TeamMemberState, starts the timer that ends it (0xA70710: the server sets the replicated one
    // back to None, a client its TeamMemberState) and tells the HUD (its team member state delegate).
    DefineUProperty(uint8_t, ReplicatedTeamMemberState)
    DefineUFunction(OnRep_ReplicatedTeamMemberState)
    static bool HasTeamMemberState() { return Member_ReplicatedTeamMemberState.IsAvailable(); }
    // 21.40's AFortPlayerStateAthena::SetTeamMemberState, server only: the callout, replicated, its OnRep run here for
    // the timer that ends it.
    void SetTeamMemberState(uint8_t NewState);

    // The player's team, copied from its AFortTeamInfo when it joins one (0x44AB80).
    DefineUProperty(EFortTeam, TeamIndex)
    // Set when the player's team is out of the match (the teams left, for each player of it: 0x44A870) or has won (1).
    DefineUProperty(int32_t, Place)
    // Other teams' players the player eliminated: Kills up to 1.7.2, KillScore from 1.8 (GetKills). 1.7.2's death
    // handler adds to Kills (0x437DB0); 1.8's adds to nothing (KillScore's only writer copies another player state's,
    // 0x4216C0), so Core counts 1.8's kills itself (CountsKills; AFortGameModeAthena's results).
    DefineUProperty(int32_t, Kills)
    DefineUProperty(int32_t, KillScore)
    // KillScore's RepNotify. From the playlists with a ScoringData (Score Royale's FirstToGoalScore), 5.30's (0xC4FA60)
    // also scores the eliminations: the playlist's Elimination points for each, into TotalPlayerScore and the team's score.
    DefineUFunction(OnRep_Kills)
    // The kills of the player's whole team (not replicated): 21.40's AFortPlayerStateAthena::SetKillScore keeps it for
    // each of the team's players.
    DefineUProperty(int32_t, TeamKillScore)
    // Score Royale's (5.30): the player's points, the sum of what their scoring events gave (AFortPlayerStateAthena
    // 0xC4B5A0 counts a container's or coin's, 0xC3C460 prices it from the playlist's ScoringData) and their eliminations'
    // (0xC3C540, which adds what it gained to the player's AFortTeamInfoAthena score).
    DefineUProperty(int32_t, TotalPlayerScore)
    static bool HasTotalPlayerScore() { return Member_TotalPlayerScore.IsAvailable(); }
    DefineUStructProperty(FDeathInfo, DeathInfo)
    // 6.02: the respawn place of the death in DeathInfo (FFortRespawnData).
    DefineUStructProperty(FFortRespawnData, RespawnData)
    static bool HasRespawnData() { return Member_RespawnData.IsAvailable() && FFortRespawnData::IsReadable(); }
    // 1.11: the player's squad within their team, 0 for none (OnRep_SquadId adds the squad's player states to the
    // owning controller's SquadMembers). AFortGameModeAthena::PlacePlayerOnTeam (2.1 0x444780) sets it from the
    // dedicated session's squads, which the match assignment's teams[i] made squad i + 1 (0x738440).
    DefineUProperty(uint8_t, SquadId)
    static bool HasSquadId() { return Member_SquadId.IsAvailable(); }
    // 1.10: the other teams' players the player knocked down (the DBNO handling adds to it), and those they hit (the
    // hit's stat, 0x43D8E0, adds each to the set), for the end-of-match screen's Downs and Assists.
    DefineUProperty(int32_t, DownScore)
    static bool HasDownScore() { return Member_DownScore.IsAvailable(); }
    // 3.3: the player's map marker, the world X and Y their squad's map and compass show (OnRep_MapIndicatorPos).
    // AFortPlayerControllerAthena::ServerPlaceMapCursor (3.3 0xA2E850) sets it, ServerRemoveMapCursor (0xA2E9B0) zeroes
    // it, and GetMapIndicatorLocation (0xA1F280) reads it back.
    DefineUProperty(FVector2D, MapIndicatorPos)
    static bool HasMapIndicatorPos() { return Member_MapIndicatorPos.IsAvailable(); }
    // 5.30: the player thanked the bus driver, replicated to everyone. AFortPlayerControllerAthena::
    // ServerThankBusDriver_Implementation (5.30 0xC289A0) only sets it, on its PlayerState when that is an
    // AFortPlayerStateAthena; each client's OnRep_ThankedBusDriver (0xC510B0) puts "<{VictimStyle}>{PlayerName}</> has
    // thanked the bus driver" (Athena.ThankedBusDriver, TeamVictim for a teammate) in its feed. One bit of a byte of
    // flags on every build that has it (5.30 to 6.01: bit 1, after bInAircraft's), so it is read and set by its mask:
    // as a whole byte it read true for anyone in the aircraft and its store set bInAircraft instead.
    DefineBitfieldUProperty(bThankedBusDriver)
    static bool HasThankedBusDriver() { return Member_bThankedBusDriver.IsAvailable(); }

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
    DefineUStructProperty(FFortSpectatorZoneArray, Spectators)
    static bool HasSpectators() { return Member_SpectatingTarget.IsAvailable() && Member_Spectators.IsAvailable() && FFortSpectatorZoneArray::IsWritable(); }

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

    // 21.40's static AFortPlayerStateAthena::ToDeathCause: the EDeathCause the damage tags give (DeathInfo.DeathCause,
    // which picks the kill feed line), DBNOTimeout for some when the pawn was DBNO. From 3.5 it is that static;
    // up to 3.3 the pawn's own (DeadPawn). Only where HasToDeathCause.
    static bool HasToDeathCause() { return Natives::AFortPlayerStateAthena_ToDeathCause || Natives::AFortPlayerPawn_GetDeathCause; }
    static uint8_t ToDeathCause(AFortPawn* DeadPawn, const FGameplayTagContainer& InTags, bool bWasDBNO)
    {
        if (Natives::AFortPlayerStateAthena_ToDeathCause)
        {
            return Natives::AFortPlayerStateAthena_ToDeathCause(InTags, bWasDBNO);
        }
        return Natives::AFortPlayerPawn_GetDeathCause(DeadPawn, InTags, bWasDBNO);
    }

    static bool HasKillScore() { return Member_KillScore.IsAvailable(); }
    static bool HasTeamKillScore() { return Member_TeamKillScore.IsAvailable(); }

    static bool HasEliminator()
    {
        return Member_DeathInfo.IsAvailable() && FDeathInfo::IsReadable();
    }

    // The player state's own store of a death (6.02 0xC4A090, 6.01 0xC478B0; the 21.40 exports don't name it, the name is
    // Core's): InDeathInfo copied whole over DeathInfo and bInitialized set, which is what makes it a death to the game's
    // own code (IsPlayerDead, IsRespawningAllowed's storm rule, FinishRestartPlayer's move to the respawn place). The
    // build's own function, called; only where Finder::ResolveSetDeathInfoLive found it (from 6.01).
    static bool HasSetDeathInfo() { return Natives::AFortPlayerStateAthena_SetDeathInfo != nullptr && Member_DeathInfo.IsAvailable() && FDeathInfo::StaticStruct() != nullptr; }
    void SetDeathInfo(const FDeathInfo& InDeathInfo) { Natives::AFortPlayerStateAthena_SetDeathInfo(this, InDeathInfo); }

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
    // team's KillScore). Replicated at once (ForceNetUpdate), so the HUD's kill count (OnRep_Kills) follows. Builds
    // with Kills (1.7.2) count their own: nothing is set there. A RepNotify runs on clients only, and a scoring
    // playlist's elimination points are scored in OnRep_Kills, so there the server, whose points count, calls it itself
    // (the Unreal way to run a RepNotify on the server).
    void SetKillScore(int32_t NewKillScore);

    // Whether the match's playlist scores its players' events: Score Royale's FirstToGoalScore (IsScoringPlaylist).
    bool IsScoringMatch();
    // Score Royale's scoring of an event with these tags (5.30 0xC4B5A0, 5.40 0xC5A4F0; the 21.40 exports don't name it,
    // the name is Core's): each of the playlist's ScoringData entries whose EventInclusionTags the tags match counts the
    // event (NumChestsOpened, NumAmmoCansOpened, ...), the points go into TotalPlayerScore and the team's score, which
    // AFortGameModeAthena's Tick checks against the GoalScore. The build's own; the server only. AFortPlayerController::
    // ServerAttemptInteract_Implementation calls it with the receiving actor's own tags once an interaction succeeds
    // (a chest's Building.Type.Container.TreasureChest, an ammo box's .Ammobox), so a player bot's search does too.
    void HandleScoringEvent(const FGameplayTagContainer& EventTags);
};
