#pragma once

#include "Player/FortPlayerController.h"
#include "UObject/WeakObjectPtr.h"

class AFortPlayerState;

// What a player's client is told of their death (ClientOnPawnDied), which the controller keeps as PlayerDeathReport.
// PrepareDeathReport (1.10 0x8DB450) fills the killer from the death's instigating controller: none for a fall or the
// storm. The 1.10 elimination screen (EliminatedOverlay) shows KillerPlayerState's Banner.
struct FFortPlayerDeathReport
{
    DefineUnrealStruct(FFortPlayerDeathReport, FortniteGame)
    DefineUProperty(AFortPlayerState*, KillerPlayerState)

public:
    static ptrdiff_t KillerPlayerStateOffset() { return Member_KillerPlayerState.GetOffsetChecked(); }
};

class AFortPlayerControllerZone : public AFortPlayerController
{
    DefineUnrealClass(AFortPlayerControllerZone, FortniteGame)
    DefineUFunction(ServerTriggerSetupTeleportCameraEvent)

public:
    void ServerTriggerSetupTeleportCameraEvent_Implementation();
    // The client's ServerAcknowledgePossession, sent once it has taken a new pawn (APlayerController::ClientRestart).
    void ServerAcknowledgePossession_Implementation(APawn* P);

    static void HookVTables();

private:
    static inline FWeakObjectPtr SetupTeleportOutCameraGameState;

    static void ServerTriggerSetupTeleportCameraEvent_ImplementationHK(AFortPlayerControllerZone* This);
    // Restores ServerAcknowledgePossession_Implementation where the build strips this class's override (5.0).
    static void HookPossessionVTables();
    static void ServerAcknowledgePossession_ImplementationHK(AFortPlayerControllerZone* This, APawn* P);
    // The super class's (AFortPlayerController's, which is APlayerController's) ServerAcknowledgePossession_Implementation.
    static inline void (*ServerAcknowledgePossession_ImplementationSuper)(AFortPlayerControllerZone* This, APawn* P) = nullptr;
};
