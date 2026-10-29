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
};

class APartyBeaconHost : public AActor
{
    DefineUnrealClass(APartyBeaconHost, OnlineSubsystemUtils)

public:
    DefineUProperty(UPartyBeaconState*, State)

    static bool IsReflected() { return Member_State.IsAvailable() && UPartyBeaconState::IsReflected(); }
};
