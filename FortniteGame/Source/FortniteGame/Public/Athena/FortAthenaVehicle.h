#pragma once

#include "GameFramework/Pawn.h"
#include "Math/Quat.h"
#include "Math/Vector.h"

// The physics state a vehicle's driver sends the server (4.4.2: 0x40 bytes).
struct alignas(16) FReplicatedAthenaVehiclePhysicsState
{
    FVector Translation;
    FQuat Rotation;
    FVector LinearVelocity;
    FVector AngularVelocity;
    uint16_t SyncKey = 0;
};
static_assert(sizeof(FReplicatedAthenaVehiclePhysicsState) == 0x40);

// Battle Royale's vehicles (the shopping cart and its kin, 4.x+). The driver's client simulates the vehicle and sends
// its body's state through ServerUpdatePhysicsParams.
class AFortAthenaVehicle : public APawn
{
    DefineUnrealClass(AFortAthenaVehicle, FortniteGame)
    DefineUFunction(ServerUpdatePhysicsParams)

public:
    void ServerUpdatePhysicsParams_Implementation(const FReplicatedAthenaVehiclePhysicsState& InState);

    static void HookVTables();

private:
    static void ServerUpdatePhysicsParams_ImplementationHK(AFortAthenaVehicle* This, const FReplicatedAthenaVehiclePhysicsState& InState);
};
