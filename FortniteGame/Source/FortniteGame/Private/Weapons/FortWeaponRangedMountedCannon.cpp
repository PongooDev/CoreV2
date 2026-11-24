#include "pch.h"
#include "Weapons/FortWeaponRangedMountedCannon.h"
#include "Athena/FortAthenaSKPushCannon.h"
#include "Finder.h"
#include "Hooks.h"
#include "Log.h"
#include "Natives.h"
#include "Pawns/FortPlayerPawn.h"
#include "ReflectionParams.h"
#include "Templates/Casts.h"
#include "UObject/UObjectIterator.h"

namespace
{
    // AFortPlayerPawn's GetVehicle and ServerOnExitVehicle and AFortAthenaVehicle's GetPawnAtSeat: intact natives
    // (8.30 ServerOnExitVehicle_Implementation 0x15F68F0).
    UFunction* GetVehicleFunction = nullptr;
    UFunction* ServerOnExitVehicleFunction = nullptr;
    UFunction* GetPawnAtSeatFunction = nullptr;

    template <typename T>
    T* CallObjectGetter(UObject* Object, UFunction* Function, const wchar_t* Parameter = nullptr, int32_t Value = 0)
    {
        if (!Object || !Function)
        {
            return nullptr;
        }
        Reflection::FParams Params(Function, true);
        if (Parameter && !Params.Set<int32_t>(Parameter, Value))
        {
            return nullptr;
        }
        Params.Call(Object);
        UObject** Result = Params.Get<UObject*>(L"ReturnValue");
        return Result ? Cast<T>(*Result) : nullptr;
    }

    // OnPreLaunchPawn and OnLaunchPawn are BlueprintEvents: their thunk calls ProcessEvent(FindFunctionChecked(NAME_...)),
    // which finds PushCannon_C's override. AFortAthenaSKPushCannon's own UFunction has an empty script, so calling it
    // directly ran no launch (#623: the passenger only exited).
    void SendLaunchEvent(AFortAthenaSKPushCannon* Cannon, UFunction* Event, AFortPlayerPawn* Pawn, const FVector& LaunchDir)
    {
        UFunction* Override = Cannon && Event ? Cannon->FindFunction(Event->GetFName()) : nullptr;
        if (!Override)
        {
            return;
        }
        Reflection::FParams Params(Override, true);
        Params.Set<AFortPlayerPawn*>(L"Pawn", Pawn);
        Params.Set<FVector>(L"LaunchDir", LaunchDir);
        Params.Call(Cannon);
    }
}

// 8.20's body (0x172C1D0 -> 0x172D340 with bServer): the weapon's pawn pushes an AFortAthenaSKPushCannon with someone
// in seat 1; on the server that passenger is fired (0xE22D50): the cannon's OnPreLaunchPawn, the passenger's
// ServerOnExitVehicle, OnLaunchPawn (the Blueprint's launch), MultiCastPushCannonLaunchedPlayer. 8.30 (CL-5793395) adds
// LaunchDir (the client's aim, 0x17EC830) to the RPC and both launch events, and strips the body: slot 0x14F of
// AFortWeaponRangedMountedCannon_VFT (0x4272DD8) is the shared empty function, and the fire helpers are gone.
void AFortWeaponRangedMountedCannon::ServerFireActorInCannon_Implementation(const FVector& LaunchDir)
{
#if WITH_SERVER_CODE
    AFortPlayerPawn* Pusher = Role == ROLE_Authority ? Cast<AFortPlayerPawn>(reinterpret_cast<UObject*>(static_cast<APawn*>(Instigator))) : nullptr;
    AFortAthenaSKPushCannon* Cannon = CallObjectGetter<AFortAthenaSKPushCannon>(Pusher, GetVehicleFunction);
    AFortPlayerPawn* Passenger = CallObjectGetter<AFortPlayerPawn>(Cannon, GetPawnAtSeatFunction, L"SeatIdx", 1);
    if (!Passenger)
    {
        return;
    }
    SendLaunchEvent(Cannon, AFortAthenaSKPushCannon::FindStaticFunction_OnPreLaunchPawn(), Passenger, LaunchDir);
    if (ServerOnExitVehicleFunction)
    {
        Reflection::FParams Params(ServerOnExitVehicleFunction, true);
        Params.Call(Passenger);
    }
    SendLaunchEvent(Cannon, AFortAthenaSKPushCannon::FindStaticFunction_OnLaunchPawn(), Passenger, LaunchDir);
    if (UFunction* Launched = AFortAthenaSKPushCannon::FindStaticFunction_MultiCastPushCannonLaunchedPlayer())
    {
        Reflection::FParams Params(Launched, true);
        Params.Call(Cannon);
    }
#endif
}

void AFortWeaponRangedMountedCannon::ServerFireActorInCannon_ImplementationHK(AFortWeaponRangedMountedCannon* This, const FVector& LaunchDir)
{
    This->ServerFireActorInCannon_Implementation(LaunchDir);
}

void AFortWeaponRangedMountedCannon::HookVTables()
{
#if WITH_SERVER_CODE
    // Only the 8.30 RPC (with LaunchDir), and only where its slot is the shared empty function: 7.40 to 8.20's
    // ServerFireActorInCannon() keeps its body (8.20 0x172C1D0).
    UFunction* Function = StaticClass() ? FindStaticFunction_ServerFireActorInCannon() : nullptr;
    if (!Function || !Function->FindPropertyByName(FName(L"LaunchDir", FNAME_Find)))
    {
        return;
    }
    UObject* Default = StaticClass()->GetDefaultObject(false);
    void** DefaultVTable = Default ? *reinterpret_cast<void***>(Default) : nullptr;
    const int32_t Index = Finder::ImplementationIndex(Function);
    if (!DefaultVTable || Index < 0 || DefaultVTable[Index] != Natives::SharedEmptyFunction)
    {
        Log::Info(L"Finder: AFortWeaponRangedMountedCannon::ServerFireActorInCannon_Implementation is native in this build");
        return;
    }
    UClass* PlayerPawnClass = AFortPlayerPawn::StaticClass();
    GetVehicleFunction = PlayerPawnClass ? PlayerPawnClass->FindFunctionByName(FName(L"GetVehicle", FNAME_Find)) : nullptr;
    ServerOnExitVehicleFunction = PlayerPawnClass ? PlayerPawnClass->FindFunctionByName(FName(L"ServerOnExitVehicle", FNAME_Find)) : nullptr;
    UClass* VehicleClass = AFortAthenaVehicle::StaticClass();
    GetPawnAtSeatFunction = VehicleClass ? VehicleClass->FindFunctionByName(FName(L"GetPawnAtSeat", FNAME_Find)) : nullptr;
    if (!GetVehicleFunction || !ServerOnExitVehicleFunction || !GetPawnAtSeatFunction || !AFortAthenaSKPushCannon::StaticClass()
        || !AFortAthenaSKPushCannon::FindStaticFunction_OnLaunchPawn())
    {
        Log::Error(L"Finder: AFortWeaponRangedMountedCannon::ServerFireActorInCannon_Implementation is stripped and the cannon's functions weren't found, so the Pirate Cannon fires nobody");
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
            Hooks::SwapVTable(L"AFortWeaponRangedMountedCannon::ServerFireActorInCannon_Implementation", VTable, Index,
                reinterpret_cast<void*>(&ServerFireActorInCannon_ImplementationHK));
            ++Swapped;
        }
    }
    Log::Info(L"Finder: AFortWeaponRangedMountedCannon::ServerFireActorInCannon_Implementation (slot 0x{:X}) is stripped, restored in {} classes", Index, Swapped);
#endif
}
