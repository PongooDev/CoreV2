#include "pch.h"
#include "Player/FortPlayerControllerPvE.h"
#include "Finder.h"
#include "Hooks.h"
#include "Log.h"
#include "Natives.h"
#include "ReflectionParams.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"

void AFortPlayerControllerPvE::ServerTriggerSetupTeleportCameraEvent_Implementation()
{
    // Only the declaring class changed: 8.00's UFortResultsWidget::TriggerSetupTeleportCameraEvent (0x32DB740) now
    // casts the owning player to this class before calling the RPC.
    AFortPlayerControllerZone::ServerTriggerSetupTeleportCameraEvent_Implementation();
}

void AFortPlayerControllerPvE::ServerUpdateQuickBarRecordsCache_Implementation(const FQuickBar& PrimaryQuickBarRecord, const FQuickBar& SecondaryQuickBarRecord)
{
    // Only the declaring class changed (8.20): the client's HandleQuickBarChanged sends it from this class.
    AFortPlayerController::ServerUpdateQuickBarRecordsCache_Implementation(PrimaryQuickBarRecord, SecondaryQuickBarRecord);
}

void AFortPlayerControllerPvE::ClientUpdateQuickBarRecordsCache(const FQuickBar& PrimaryQuickBarRecord, const FQuickBar& SecondaryQuickBarRecord)
{
    Reflection::FParams Params(StaticFunction_ClientUpdateQuickBarRecordsCache(), true);
    Params.CopyStruct(L"PrimaryQuickBarRecord", &PrimaryQuickBarRecord);
    Params.CopyStruct(L"SecondaryQuickBarRecord", &SecondaryQuickBarRecord);
    Params.Call(this);
}

void AFortPlayerControllerPvE::HookVTables()
{
#if WITH_SERVER_CODE
    HookQuickBarRecordVTables();

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

void AFortPlayerControllerPvE::ServerUpdateQuickBarRecordsCache_ImplementationHK(AFortPlayerControllerPvE* This, const FQuickBar& PrimaryQuickBarRecord, const FQuickBar& SecondaryQuickBarRecord)
{
    This->ServerUpdateQuickBarRecordsCache_Implementation(PrimaryQuickBarRecord, SecondaryQuickBarRecord);
}

void AFortPlayerControllerPvE::HookQuickBarRecordVTables()
{
#if WITH_SERVER_CODE
    // 7.40 and 8.00 declare the RPC on AFortPlayerController, which restores it there.
    UFunction* const Function = FindStaticFunction_ServerUpdateQuickBarRecordsCache();
    if (!Function || Function->GetOuter() != reinterpret_cast<UObject*>(StaticClass()) || !HasQuickBarRecordsCache())
    {
        return;
    }
    // 8.40 CL-6005771: exec 0x1A36D70 calls slot 0x3EF, the empty 0x310BA0 in this class's vtable, as
    // AFortPlayerController's slot 0x20F was in 7.40.
    const int32_t Index = Finder::ImplementationIndex(Function);
    if (Index < 0)
    {
        Log::Error(L"Finder: AFortPlayerControllerPvE::ServerUpdateQuickBarRecordsCache_Implementation slot not found");
        return;
    }

    int32_t Restored = 0;
    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Class = *It;
        UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
        void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
        if (VTable && Finder::IsStubFunction(reinterpret_cast<uintptr_t>(VTable[Index])))
        {
            Restored += Hooks::SwapVTable(L"AFortPlayerControllerPvE::ServerUpdateQuickBarRecordsCache_Implementation", VTable, Index, reinterpret_cast<void*>(&ServerUpdateQuickBarRecordsCache_ImplementationHK)) ? 1 : 0;
        }
    }
    Log::Info(L"Finder: AFortPlayerControllerPvE::ServerUpdateQuickBarRecordsCache_Implementation (slot 0x{:X}) {}", Index,
        Restored > 0 ? std::format(L"is stripped, restored in {} vtables", Restored) : std::wstring(L"is native in this build"));
#endif
}
