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

    static void HookVTables();

private:
    static inline FWeakObjectPtr SetupTeleportOutCameraGameState;

    static void ServerTriggerSetupTeleportCameraEvent_ImplementationHK(AFortPlayerControllerZone* This);
};
