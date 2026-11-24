#pragma once

#include "Player/FortPlayerControllerZone.h"

class AFortPlayerControllerPvE : public AFortPlayerControllerZone
{
    DefineUnrealClass(AFortPlayerControllerPvE, FortniteGame)
    // 8.00 (CL-5203069) moves this RPC here from AFortPlayerControllerZone (7.40 and older declare it there).
    DefineUFunction(ServerTriggerSetupTeleportCameraEvent)
    // 8.20 (CL-5625478) moves the client quickbars' layout records and their RPCs here from AFortPlayerController (7.40
    // and 8.00 declare them there; these lookups then find those through the super class).
    DefineUStructProperty(FQuickBar, CachedPrimaryQuickBarRecord)
    DefineUStructProperty(FQuickBar, CachedSecondaryQuickBarRecord)
    DefineUFunction(ClientUpdateQuickBarRecordsCache)
    DefineUFunction(ServerUpdateQuickBarRecordsCache)

public:
    static bool HasQuickBarRecordsCache() { return Member_CachedPrimaryQuickBarRecord.IsAvailable() && Member_CachedSecondaryQuickBarRecord.IsAvailable(); }

    void ServerTriggerSetupTeleportCameraEvent_Implementation();
    // 8.20+: the client's layout, as AFortPlayerController's in 7.40 and 8.00. The _Implementation is stripped.
    void ServerUpdateQuickBarRecordsCache_Implementation(const FQuickBar& PrimaryQuickBarRecord, const FQuickBar& SecondaryQuickBarRecord);
    void ClientUpdateQuickBarRecordsCache(const FQuickBar& PrimaryQuickBarRecord, const FQuickBar& SecondaryQuickBarRecord);

    static void HookVTables();

private:
    static void ServerTriggerSetupTeleportCameraEvent_ImplementationHK(AFortPlayerControllerPvE* This);
    static void HookQuickBarRecordVTables();
    static void ServerUpdateQuickBarRecordsCache_ImplementationHK(AFortPlayerControllerPvE* This, const FQuickBar& PrimaryQuickBarRecord, const FQuickBar& SecondaryQuickBarRecord);
};
