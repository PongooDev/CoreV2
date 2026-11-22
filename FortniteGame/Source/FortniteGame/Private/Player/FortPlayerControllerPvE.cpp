#include "pch.h"
#include "Player/FortPlayerControllerPvE.h"
#include "Finder.h"
#include "Hooks.h"
#include "Log.h"
#include "Natives.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"

void AFortPlayerControllerPvE::ServerTriggerSetupTeleportCameraEvent_Implementation()
{
    // Only the declaring class changed: 8.00's UFortResultsWidget::TriggerSetupTeleportCameraEvent (0x32DB740) now
    // casts the owning player to this class before calling the RPC.
    AFortPlayerControllerZone::ServerTriggerSetupTeleportCameraEvent_Implementation();
}

void AFortPlayerControllerPvE::HookVTables()
{
#if WITH_SERVER_CODE
    // Builds before 8.00 declare the RPC on AFortPlayerControllerZone (the lookup finds it through the super class),
    // which restores it there.
    UFunction* const Function = FindStaticFunction_ServerTriggerSetupTeleportCameraEvent();
    if (!Function || Function->GetOuter() != reinterpret_cast<UObject*>(StaticClass()))
    {
        return;
    }
    // 8.00 CL-5203069: exec 0x1819680 calls slot 0x404, the shared empty function in this class and Outpost's.
    // Without it the mission never gets SetupTeleportOutCamera, and the results screen's Return to Homebase
    // never leaves the zone (#549).
    const int32_t Index = Finder::ImplementationIndex(Function);
    if (Index < 0)
    {
        Log::Error(L"Finder: AFortPlayerControllerPvE::ServerTriggerSetupTeleportCameraEvent_Implementation slot not found");
        return;
    }

    int32_t Swapped = 0;
    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Class = *It;
        UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
        void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
        if (VTable && VTable[Index] == Natives::SharedEmptyFunction)
        {
            Swapped += Hooks::SwapVTable(L"AFortPlayerControllerPvE::ServerTriggerSetupTeleportCameraEvent_Implementation", VTable, Index, reinterpret_cast<void*>(&ServerTriggerSetupTeleportCameraEvent_ImplementationHK)) ? 1 : 0;
        }
    }
    if (Swapped == 0)
    {
        Log::Info(L"Finder: AFortPlayerControllerPvE::ServerTriggerSetupTeleportCameraEvent_Implementation (slot 0x{:X}) is native in this build", Index);
    }
#endif
}

void AFortPlayerControllerPvE::ServerTriggerSetupTeleportCameraEvent_ImplementationHK(AFortPlayerControllerPvE* This)
{
    This->ServerTriggerSetupTeleportCameraEvent_Implementation();
}
