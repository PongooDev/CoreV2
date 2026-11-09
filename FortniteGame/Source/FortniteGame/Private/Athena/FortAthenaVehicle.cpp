#include "pch.h"
#include "Athena/FortAthenaVehicle.h"
#include "Components/PrimitiveComponent.h"
#include "Finder.h"
#include "Hooks.h"
#include "Log.h"
#include "Math/Rotator.h"
#include "Natives.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"

// Leaving a vehicle put the player back where they got in (#332). The driver's client simulates the vehicle and sends
// its body's state through ServerUpdatePhysicsParams, but 4.4.2 strips the server's side: AFortAthenaVehicle's slot
// 0x730 (the exec, 0x1164370, calls it after ServerUpdatePhysicsParams_Validate in 0x728) holds the shared empty
// function. So the vehicle never moved on the server, the riders attached to it stayed where they got in, and the
// exit (ServerOnExitVehicle) placed the player next to the server's vehicle, there.
void AFortAthenaVehicle::ServerUpdatePhysicsParams_Implementation(const FReplicatedAthenaVehiclePhysicsState& InState)
{
#if WITH_SERVER_CODE
    // The state is the driver's body's: teleport the body there (keeping its simulation), then give it the velocities.
    SetActorLocationAndRotation(InState.Translation, InState.Rotation.Rotator(), false, true);
    USceneComponent* Root = GetRootComponent();
    if (Root && Root->IsA(UPrimitiveComponent::StaticClass()))
    {
        UPrimitiveComponent* Body = static_cast<UPrimitiveComponent*>(Root);
        Body->SetPhysicsLinearVelocity(InState.LinearVelocity);
        Body->SetPhysicsAngularVelocityInDegrees(InState.AngularVelocity);
    }
#endif
}

void AFortAthenaVehicle::HookVTables()
{
    // Builds before 4.x have no vehicles.
    UFunction* ServerUpdatePhysicsParams = FindStaticFunction_ServerUpdatePhysicsParams();
    if (!ServerUpdatePhysicsParams)
    {
        return;
    }

    const int32_t Index = Finder::ImplementationIndex(ServerUpdatePhysicsParams);
    UObject* VehicleDefault = StaticClass()->GetDefaultObject(false);
    void** VehicleVTable = VehicleDefault ? *reinterpret_cast<void***>(VehicleDefault) : nullptr;
    if (Index < 0 || !VehicleVTable)
    {
        Log::Error(L"Finder: AFortAthenaVehicle::ServerUpdatePhysicsParams_Implementation slot not found");
        return;
    }
    if (VehicleVTable[Index] != Natives::SharedEmptyFunction)
    {
        Log::Info(L"Finder: AFortAthenaVehicle::ServerUpdatePhysicsParams_Implementation (slot 0x{:X}) is native in this build", Index);
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
            Hooks::SwapVTable(L"AFortAthenaVehicle::ServerUpdatePhysicsParams_Implementation", VTable, Index, reinterpret_cast<void*>(&ServerUpdatePhysicsParams_ImplementationHK));
            ++Swapped;
        }
    }
    Log::Info(L"Finder: AFortAthenaVehicle::ServerUpdatePhysicsParams_Implementation slot 0x{:X}, restored in {} vehicle classes", Index, Swapped);
}

void AFortAthenaVehicle::ServerUpdatePhysicsParams_ImplementationHK(AFortAthenaVehicle* This, const FReplicatedAthenaVehiclePhysicsState& InState)
{
    This->ServerUpdatePhysicsParams_Implementation(InState);
}
