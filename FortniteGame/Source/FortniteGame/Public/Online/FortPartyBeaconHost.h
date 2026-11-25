#pragma once

#include "Containers/Array.h"
#include "GameFramework/Actor.h"
#include "Math/UnrealMathUtility.h"
#include "Templates/Sorting.h"
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
    DefineUStructProperty(FUniqueNetIdRepl, UniqueId)

    static bool IsReflected() { return Member_UniqueId.IsAvailable(); }
};

// A party's reservation: its players, all on TeamNum (the beacon's team index; the player's EFortTeam is TeamNum + 2,
// HumanPvP_Team1 for 0).
struct FPartyReservation
{
    DefineUnrealStruct(FPartyReservation, OnlineSubsystemUtils)
    DefineUProperty(int32_t, TeamNum)
    DefineUStructProperty(FUniqueNetIdRepl, PartyLeader)
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

    // GetTeamAssignment's Smallest method (ETeamAssignmentMethod::Smallest; 8.51 CL-6165369 0x630230): of the teams with
    // room for the party's members, the one with the fewest players; INDEX_NONE when none has room. Equal teams are put
    // in a random order (FSortTeamSizeSmallestToLargest, 0x616FA0: rand() % 2 on equal sizes), so the first player of a
    // two-team game isn't always on the first team (Endgame's villains are team 0, CoreV2 #651). The match's beacon
    // uses Manual (AFortPartyBeaconHost's ConfigureForMatchAssignment), keeping the matchmaking service's teams, so this
    // is for a reservation Core makes itself.
    int32_t GetTeamAssignment(const FPartyReservation& Party) const
    {
        struct FTeamBalanceInfo
        {
            int32_t TeamIdx;
            int32_t CurrentSize;
        };
        struct FSortTeamSizeSmallestToLargest
        {
            bool operator()(const FTeamBalanceInfo& A, const FTeamBalanceInfo& B) const
            {
                return A.CurrentSize == B.CurrentSize ? FMath::Rand() % 2 != 0 : A.CurrentSize < B.CurrentSize;
            }
        };
        TArray<FTeamBalanceInfo> PotentialTeamChoices;
        for (int32_t TeamIdx = 0; TeamIdx < NumTeams; ++TeamIdx)
        {
            const int32_t CurrentPlayersOnTeam = GetNumPlayersOnTeam(TeamIdx);
            if (CurrentPlayersOnTeam + Party.GetMember_PartyMembers().Num() <= NumPlayersPerTeam)
            {
                PotentialTeamChoices.Add({ TeamIdx, CurrentPlayersOnTeam });
            }
        }
        if (PotentialTeamChoices.Num() == 0)
        {
            return INDEX_NONE;
        }
        Sort(PotentialTeamChoices.GetData(), PotentialTeamChoices.Num(), FSortTeamSizeSmallestToLargest());
        return PotentialTeamChoices[0].TeamIdx;
    }
};

class APartyBeaconHost : public AActor
{
    DefineUnrealClass(APartyBeaconHost, OnlineSubsystemUtils)

public:
    DefineUProperty(UPartyBeaconState*, State)

    static bool IsReflected() { return Member_State.IsAvailable() && UPartyBeaconState::IsReflected(); }
};
