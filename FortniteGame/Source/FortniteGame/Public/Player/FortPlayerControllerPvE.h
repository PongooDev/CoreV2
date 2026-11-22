#pragma once

#include "Player/FortPlayerControllerZone.h"

class AFortPlayerControllerPvE : public AFortPlayerControllerZone
{
    DefineUnrealClass(AFortPlayerControllerPvE, FortniteGame)
    // 8.00 (CL-5203069) moves this RPC here from AFortPlayerControllerZone (7.40 and older declare it there).
    DefineUFunction(ServerTriggerSetupTeleportCameraEvent)

public:
    void ServerTriggerSetupTeleportCameraEvent_Implementation();

    static void HookVTables();

private:
    static void ServerTriggerSetupTeleportCameraEvent_ImplementationHK(AFortPlayerControllerPvE* This);
};
