#pragma once

#include "GameFramework/Pawn.h"
#include "Math/Quat.h"
#include "Math/Vector.h"

// The physics state a vehicle's driver sends the server (4.2-5.0: 0x40 bytes). The driver's client fills it from its
// body (4.1 0xA381C0): Translation and Rotation are the body's ComponentToWorld, LinearVelocity is
// GetPhysicsLinearVelocity and AngularVelocity is GetPhysicsAngularVelocityInRadians (radians per second). 4.4.2 and
// 4.5 send Rotation encoded (see ReceivedRotation in FortAthenaVehicle.cpp).
struct alignas(16) FReplicatedAthenaVehiclePhysicsState
{
    FVector Translation;
    FQuat Rotation;
    FVector LinearVelocity;
    FVector AngularVelocity;
    uint16_t SyncKey = 0;
};
static_assert(sizeof(FReplicatedAthenaVehiclePhysicsState) == 0x40);

// The driver's input (throttle, steering, sprint, jump, look direction). Its layout changes between builds (4.2: 0x14
// bytes, 4.4-4.5: 0x24), so Core copies it by the reflected size and never reads its fields.
struct FReplicatedAthenaVehicleInputState
{
    DefineUnrealStruct(FReplicatedAthenaVehicleInputState, FortniteGame)
};

// Battle Royale's vehicles (the shopping cart, ShoppingCartVehicleSK_C : AFortAthenaSKPushVehicle, and its kin, 4.x+).
// The driver's client simulates the vehicle and sends its input and its body's state through ServerUpdateInputParams
// and ServerUpdatePhysicsParams.
class AFortAthenaVehicle : public APawn
{
    DefineUnrealClass(AFortAthenaVehicle, FortniteGame)
    DefineUFunction(ServerUpdatePhysicsParams)
    DefineUFunction(ServerUpdateInputParams)
    DefineUStructProperty(FReplicatedAthenaVehicleInputState, ReplicatedInputParams)

public:
    void ServerUpdatePhysicsParams_Implementation(const FReplicatedAthenaVehiclePhysicsState& InState);
    void ServerUpdateInputParams_Implementation(const FReplicatedAthenaVehicleInputState& InState);

    static void HookVTables();

private:
    static void ServerUpdatePhysicsParams_ImplementationHK(AFortAthenaVehicle* This, const FReplicatedAthenaVehiclePhysicsState& InState);
    static void ServerUpdateInputParams_ImplementationHK(AFortAthenaVehicle* This, const FReplicatedAthenaVehicleInputState& InState);
};
