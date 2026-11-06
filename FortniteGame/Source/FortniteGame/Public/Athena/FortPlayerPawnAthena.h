#pragma once

#include "Athena/FortPlayerControllerAthena.h"
#include "Pawns/FortPlayerPawn.h"
#include "UObject/Stack.h"

class AActor;
class UPrimitiveComponent;

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

    bool HasCustomizationLoadout() { return Member_CustomizationLoadout.IsAvailable(); }

    // 1.8.1's auto pickup (0x42A570), restored where the build strips it (1.8.2): the capsule's overlap with ammo, a
    // trap or a resource picks it up.
    void OnCapsuleBeginOverlap(UPrimitiveComponent* OverlappedComp, AActor* OtherActor, UPrimitiveComponent* OtherComp, int32_t OtherBodyIndex, bool bFromSweep, const void* SweepResult);

    static void HookNatives();

private:
    DECLARE_FUNCTION(execOnCapsuleBeginOverlap);
};
