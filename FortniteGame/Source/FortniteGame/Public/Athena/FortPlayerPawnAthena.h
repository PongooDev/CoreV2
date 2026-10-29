#pragma once

#include "Athena/FortPlayerControllerAthena.h"
#include "Pawns/FortPlayerPawn.h"

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
    // 1.8: the controller's loadout, copied by its Possess (0x444480). Every client sets up the glider of every pawn's
    // (OnRep_CustomizationLoadout, 0x43D4A0), and an empty one crashes them all.
    DefineUStructProperty(FFortAthenaLoadout, CustomizationLoadout)
    // How many teammates are reviving the pawn (AFortPlayerControllerAthena::ServerDBNOReviveStarted and Interrupted).
    DefineUProperty(uint8_t, DBNORevivalStacking)

    bool HasCustomizationLoadout() { return Member_CustomizationLoadout.IsAvailable(); }

    // 1.8.1's auto pickup (0x42A570), restored where the build strips it (1.8.2): the capsule's overlap with ammo, a
    // trap or a resource picks it up.
    void OnCapsuleBeginOverlap(UPrimitiveComponent* OverlappedComp, AActor* OtherActor, UPrimitiveComponent* OtherComp, int32_t OtherBodyIndex, bool bFromSweep, const void* SweepResult);

    static void HookNatives();

private:
    static void OnCapsuleBeginOverlapHK(AFortPlayerPawnAthena* This, UPrimitiveComponent* OverlappedComp, AActor* OtherActor, UPrimitiveComponent* OtherComp, int32_t OtherBodyIndex, bool bFromSweep, const void* SweepResult);
};
