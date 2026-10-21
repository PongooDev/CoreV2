#pragma once

#include "Player/FortPlayerController.h"
#include "UObject/WeakObjectPtr.h"

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
