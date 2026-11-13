#pragma once

#include "Athena/FortPlayerControllerAthena.h"
#include "Pawns/FortPlayerPawn.h"
#include "UObject/ScriptDelegates.h"
#include "UObject/Stack.h"

class AActor;
class UPrimitiveComponent;
struct FGameplayTagContainer;

// Battle Royale's player pawn (from 1.7.2; PlayerPawn_Athena_C).
class AFortPlayerPawnAthena : public AFortPlayerPawn
{
    DefineUnrealClass(AFortPlayerPawnAthena, FortniteGame)
    DefineUFunction(OnCapsuleBeginOverlap)

public:
    // From leaving the aircraft (0x415D90) until landing (0x41AB80).
    DefineUProperty(bool, bIsSkydiving)
    DefineUProperty(bool, bIsParachuteOpen)
    DefineUProperty(bool, bIsSkydivingFromBus)
    static ptrdiff_t IsSkydivingOffset() { return Member_bIsSkydiving.GetOffsetChecked(); }
    static bool HasIsSkydiving() { return Member_bIsSkydiving.IsAvailable(); }
    // 1.8: the controller's loadout, copied by its Possess (0x444480). Every client sets up the glider of every pawn's
    // (OnRep_CustomizationLoadout, 0x43D4A0), and an empty one crashes them all.
    DefineUStructProperty(FFortAthenaLoadout, CustomizationLoadout)
    // How many teammates are reviving the pawn (AFortPlayerControllerAthena::ServerDBNOReviveStarted and Interrupted).
    DefineUProperty(uint8_t, DBNORevivalStacking)
    static bool HasDBNORevivalStacking() { return Member_DBNORevivalStacking.IsAvailable(); }
    // The pawn the death camera turns to (the eliminator's), set by the controller's pawn-death handler.
    DefineUProperty(APawn*, KillerForSpectatorRotation)
    // The pawn is relevant to its whole team, not only its squad (IsNetRelevantFor, 3.2 0x9EEEA0), so its
    // teammates see it on the map wherever it is. AFortGameModeAthena::FinishRestartPlayer (0x9B6310) sets it on a player's
    // new pawn in a large team game, but not in 50v50.
    DefineUProperty(bool, bTeamMembersAlwaysRelevant)
    static bool HasTeamMembersAlwaysRelevant() { return Member_bTeamMembersAlwaysRelevant.IsAvailable(); }
    // How far the pawn fell when it last landed hard enough to be hurt (from 4.0). The falling-damage handler (5.10
    // 0xBEE1D0) sets it on the server as it applies the damage tagged Gameplay.Damage.Environment.Falling, and the death
    // report's Distance is this for a fall (the DBNO handling, 5.10 0xC11510).
    DefineUProperty(float, LastFallDistance)
    static bool HasLastFallDistance() { return Member_LastFallDistance.IsAvailable(); }
    // From 5.40: what the pawn's StartDBNO broadcasts as it goes down (5.40 0xC688B2, after it sets bIsDBNO and before
    // the DBNO ability's gameplay event). A Blueprint delegate without parameters.
    DefineUProperty(FMulticastScriptDelegate, OnPlayerStartDBNO)
    static bool HasOnPlayerStartDBNO() { return Member_OnPlayerStartDBNO.IsAvailable(); }

    // The death report's distance (FDeathInfo::Distance) as the DBNO handling (5.10 0xC11510) works it out: the eliminator's
    // pawn's distance (AActor::GetDistanceTo), or the fall's when the damage tags hold the falling damage tag.
    static float GetDeathInfoDistance(AFortPawn* Victim, APawn* FinisherPawn, const FGameplayTagContainer& DamageTags);

    bool HasCustomizationLoadout() { return Member_CustomizationLoadout.IsAvailable(); }

    // 1.8.1's auto pickup (0x42A570), restored where the build strips it (1.8.2): the capsule's overlap with ammo, a
    // trap or a resource picks it up.
    void OnCapsuleBeginOverlap(UPrimitiveComponent* OverlappedComp, AActor* OtherActor, UPrimitiveComponent* OtherComp, int32_t OtherBodyIndex, bool bFromSweep, const void* SweepResult);

    // FortPlayerPawnAthena.cpp: the server part of AFortPlayerPawn::StartDBNO (the pawn going down), which the build cuts
    // out: the items whose definition has bDropOnDBNO (5.40's The Getaway jewel) leave the downed pawn. It runs when the
    // build's StartDBNO broadcasts the pawn's OnPlayerStartDBNO (HandlePlayerStartDBNO); Core's knock for a bot
    // (FCoreAthenaBotController::EnterDBNO) calls it too.
    void StartDBNO();

    // Binds the pawn's OnPlayerStartDBNO to Core's HandlePlayerStartDBNO, once (AFortPlayerController::Possess: the pawn
    // a player plays). Nothing where HookVTables added no HandlePlayerStartDBNO.
    void BindPlayerStartDBNO();

    static void HookNatives();
    // From 5.40 (UFortWorldItemDefinition's bDropOnDBNO): Core's HandlePlayerStartDBNO, the native OnPlayerStartDBNO
    // calls. Where it can't be added, StartDBNO's slot instead, so the server part runs after the build's.
    static void HookVTables();

private:
    DECLARE_FUNCTION(execOnCapsuleBeginOverlap);
    DECLARE_FUNCTION(execHandlePlayerStartDBNO);

    static inline UFunction* HandlePlayerStartDBNOFunction = nullptr;

    static void StartDBNOHK(AFortPlayerPawnAthena* This);
    static inline void (*StartDBNOOG)(AFortPlayerPawnAthena* This) = nullptr;
};
