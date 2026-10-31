#pragma once

#include "GameFramework/Actor.h"
#include "GameFramework/OnlineReplStructs.h"
#include "Reflection.h"
#include "StructArray.h"

// The game session's reservation beacon (OnlineSubsystemUtils' APartyBeaconHost, AFortGameSession's
// ReservationBeaconHost). Battle Royale's clients never talk to it: the matchmaking service's AssignMatch fills it, one
// reservation per party on its team (AFortPartyBeaconHost::ConfigureForMatchAssignment, 1.7.2 0x751960), and the game
// mode's PickTeam (1.9 0x430A50) reads a joining player's team from it.

// One player of a party's reservation.
struct FPlayerReservation
{
    DefineUnrealStruct(FPlayerReservation, OnlineSubsystemUtils)
    DefineUProperty(FUniqueNetIdRepl, UniqueId)

    static bool IsReflected() { return Member_UniqueId.IsAvailable(); }
};

// A party's reservation: its players, all on TeamNum (the beacon's team index; the player's EFortTeam is TeamNum + 2,
// HumanPvP_Team1 for 0).
struct FPartyReservation
{
    DefineUnrealStruct(FPartyReservation, OnlineSubsystemUtils)
    DefineUProperty(int32_t, TeamNum)
    DefineUProperty(FUniqueNetIdRepl, PartyLeader)
    DefineUStructArrayProperty(FPlayerReservation, PartyMembers)

    static bool IsReflected() { return Member_TeamNum.IsAvailable() && Member_PartyLeader.IsAvailable() && Member_PartyMembers.IsAvailable() && FPlayerReservation::IsReflected(); }
};

class UPartyBeaconState : public UObject
{
    DefineUnrealClass(UPartyBeaconState, OnlineSubsystemUtils)

public:
    DefineUProperty(int32_t, NumTeams)
    DefineUProperty(int32_t, NumPlayersPerTeam)
    DefineUStructArrayProperty(FPartyReservation, Reservations)

    static bool IsReflected() { return Member_NumTeams.IsAvailable() && Member_NumPlayersPerTeam.IsAvailable() && Member_Reservations.IsAvailable() && FPartyReservation::IsReflected(); }

    // OnlineSubsystemUtils' PartyBeaconState.cpp: the players with a valid id in the reservations on TeamIdx.
    int32_t GetNumPlayersOnTeam(int32_t TeamIdx) const
    {
        int32_t Result = 0;
        const TStructArray<FPartyReservation>& AllReservations = GetMember_Reservations();
        for (int32_t ResIdx = 0; ResIdx < AllReservations.Num(); ++ResIdx)
        {
            const FPartyReservation& Reservation = AllReservations[ResIdx];
            if (Reservation.TeamNum != TeamIdx)
            {
                continue;
            }
            const TStructArray<FPlayerReservation>& PartyMembers = Reservation.GetMember_PartyMembers();
            for (int32_t PlayerIndex = 0; PlayerIndex < PartyMembers.Num(); ++PlayerIndex)
            {
                Result += PartyMembers[PlayerIndex].UniqueId.IsValid() ? 1 : 0;
            }
        }
        return Result;
    }

    // GetTeamAssignment's Smallest method (ETeamAssignmentMethod::Smallest): of the teams with room for the party's
    // members, the one with the fewest players, the lowest on a tie; INDEX_NONE when none has room. The match's beacon
    // uses Manual (AFortPartyBeaconHost's ConfigureForMatchAssignment), keeping the matchmaking service's teams, so this
    // is for a reservation Core makes itself.
    int32_t GetTeamAssignment(const FPartyReservation& Party) const
    {
        int32_t SmallestTeam = INDEX_NONE;
        int32_t SmallestTeamCount = INT32_MAX;
        for (int32_t TeamIdx = 0; TeamIdx < NumTeams; ++TeamIdx)
        {
            const int32_t CurrentPlayersOnTeam = GetNumPlayersOnTeam(TeamIdx);
            if (CurrentPlayersOnTeam + Party.GetMember_PartyMembers().Num() <= NumPlayersPerTeam && CurrentPlayersOnTeam < SmallestTeamCount)
            {
                SmallestTeam = TeamIdx;
                SmallestTeamCount = CurrentPlayersOnTeam;
            }
        }
        return SmallestTeam;
    }
};

class APartyBeaconHost : public AActor
{
    DefineUnrealClass(APartyBeaconHost, OnlineSubsystemUtils)

public:
    DefineUProperty(UPartyBeaconState*, State)

    static bool IsReflected() { return Member_State.IsAvailable() && UPartyBeaconState::IsReflected(); }
};
