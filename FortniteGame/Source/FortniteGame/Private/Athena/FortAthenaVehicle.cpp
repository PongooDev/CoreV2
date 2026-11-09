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

#include <cstring>

// Leaving a vehicle put the player back where they got in (#332). The driver's client simulates the vehicle and sends
// its body's state through ServerUpdatePhysicsParams, but the client builds strip the server's side: the exec (4.4.2
// 0x1164370, 4.5 0x116B180) calls the slot after ServerUpdatePhysicsParams_Validate, and that slot holds the shared
// empty function in every build with vehicles (4.1 AFortAthenaSMVehicle 0x11B, 4.2 0xE4, 4.4.2 and 4.5 0xE6). So the
// vehicle never moved on the server, the riders attached to it stayed where they got in, and the exit
// (ServerOnExitVehicle) placed the player next to the server's vehicle, there.
//
// The driver's client (4.1 0xA381C0) sends its body's ComponentToWorld translation and rotation, GetPhysicsLinearVelocity
// and GetPhysicsAngularVelocityInRadians (0x2477D50 reads FBodyInstance's PhysX angular velocity unconverted), and only
// when the body has moved away from the state it last sent (0xA363A0's error >= 10). The angular velocity is therefore
// radians per second: applied as degrees, the server's body turned 57 times too slowly between updates and the
// shopping cart jerked back each time the next update teleported it (4.5).
void AFortAthenaVehicle::ServerUpdatePhysicsParams_Implementation(const FReplicatedAthenaVehiclePhysicsState& InState)
{
#if WITH_SERVER_CODE
    // Teleport the body to the driver's state (keeping its simulation), then give it the driver's velocities.
    SetActorLocationAndRotation(InState.Translation, InState.Rotation.Rotator(), false, true);
    USceneComponent* Root = GetRootComponent();
    if (Root && Root->IsA(UPrimitiveComponent::StaticClass()))
    {
        UPrimitiveComponent* Body = static_cast<UPrimitiveComponent*>(Root);
        Body->SetPhysicsLinearVelocity(InState.LinearVelocity);
        Body->SetPhysicsAngularVelocityInRadians(InState.AngularVelocity);
    }
#endif
}

// The driver's input never reached the server or the other players: ServerUpdateInputParams_Implementation is stripped
// too (the exec, 4.5 0x116B090, calls slot 0xE8 after ServerUpdateInputParams_Validate; 4.1 0x11D, 4.2 0xE6, 4.4.2
// 0xE8), and it is the only writer of ReplicatedInputParams (replicated COND_SkipReplay, 4.5 0xA3BD40). Between the
// driver's physics updates the server simulated the cart with no throttle, steering or push (AFortAthenaSKPushVehicle's
// pedal and coast states read it), so it braked and drifted until the next update teleported it, and the other players'
// copies had no input to animate the riders with.
void AFortAthenaVehicle::ServerUpdateInputParams_Implementation(const FReplicatedAthenaVehicleInputState& InState)
{
#if WITH_SERVER_CODE
    const int32_t Size = FReplicatedAthenaVehicleInputState::StaticStruct()->GetPropertiesSize();
    std::memcpy(&GetMember_ReplicatedInputParams(), &InState, static_cast<size_t>(Size));
#endif
}

namespace
{
    // Swaps Implementation into every vehicle class whose slot for Function's _Implementation is the shared empty
    // function (the stripped server side). Builds before 4.x have no vehicles; builds that keep the body are left alone.
    void RestoreStrippedImplementation(const wchar_t* Name, UFunction* Function, void* Implementation)
    {
        if (!Function)
        {
            return;
        }

        const int32_t Index = Finder::ImplementationIndex(Function);
        UObject* VehicleDefault = AFortAthenaVehicle::StaticClass()->GetDefaultObject(false);
        void** VehicleVTable = VehicleDefault ? *reinterpret_cast<void***>(VehicleDefault) : nullptr;
        if (Index < 0 || !VehicleVTable)
        {
            Log::Error(L"Finder: {} slot not found", Name);
            return;
        }
        if (VehicleVTable[Index] != Natives::SharedEmptyFunction)
        {
            Log::Info(L"Finder: {} (slot 0x{:X}) is native in this build", Name, Index);
            return;
        }

        int32_t Swapped = 0;
        for (TObjectIterator<UClass> It; It; ++It)
        {
            UClass* Class = *It;
            UObject* ClassDefault = Class->IsChildOf(AFortAthenaVehicle::StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
            void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
            if (VTable && VTable[Index] == Natives::SharedEmptyFunction)
            {
                Hooks::SwapVTable(Name, VTable, Index, Implementation);
                ++Swapped;
            }
        }
        Log::Info(L"Finder: {} slot 0x{:X}, restored in {} vehicle classes", Name, Index, Swapped);
    }
}

void AFortAthenaVehicle::HookVTables()
{
    RestoreStrippedImplementation(L"AFortAthenaVehicle::ServerUpdatePhysicsParams_Implementation", FindStaticFunction_ServerUpdatePhysicsParams(), reinterpret_cast<void*>(&ServerUpdatePhysicsParams_ImplementationHK));

    // ReplicatedInputParams is a reflected struct: without it (or its struct) there is nothing to restore into.
    if (FindStaticFunction_ServerUpdateInputParams())
    {
        if (!FReplicatedAthenaVehicleInputState::StaticStruct())
        {
            Log::Error(L"Finder: FReplicatedAthenaVehicleInputState not found, ServerUpdateInputParams stays empty");
            return;
        }
        RestoreStrippedImplementation(L"AFortAthenaVehicle::ServerUpdateInputParams_Implementation", FindStaticFunction_ServerUpdateInputParams(), reinterpret_cast<void*>(&ServerUpdateInputParams_ImplementationHK));
    }
}

void AFortAthenaVehicle::ServerUpdatePhysicsParams_ImplementationHK(AFortAthenaVehicle* This, const FReplicatedAthenaVehiclePhysicsState& InState)
{
    This->ServerUpdatePhysicsParams_Implementation(InState);
}

void AFortAthenaVehicle::ServerUpdateInputParams_ImplementationHK(AFortAthenaVehicle* This, const FReplicatedAthenaVehicleInputState& InState)
{
    This->ServerUpdateInputParams_Implementation(InState);
}
