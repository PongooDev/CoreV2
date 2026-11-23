#pragma once

#include "Athena/FortAthenaVehicle.h"
#include "Math/Vector.h"
#include "UObject/WeakObjectPtr.h"

class UPrimitiveComponent;

// Where the Baller's towhook is attached, as it replicates: the component and the point and normal in its body's space
// (FVector_NetQuantize100s; 8.00 and 8.20: 0x20 bytes).
struct FNetTowhookAttachState
{
    DefineUnrealStruct(FNetTowhookAttachState, FortniteGame)

public:
    UPrimitiveComponent* Component = nullptr;
    FVector LocalLocation;
    FVector LocalNormal;
};
static_assert(sizeof(FNetTowhookAttachState) == 0x20);

// Battle Royale's Baller (8.10; OctopusVehicle_C). Its towhook is an AFortOctopusTowhookAttachableProjectile the
// vehicle's weapon fires; the vehicle's towhook model pulls it towards ReplicatedAttachState once that is set.
class AFortOctopusVehicle : public AFortAthenaVehicle
{
    DefineUnrealClass(AFortOctopusVehicle, FortniteGame)
    DefineUFunction(ServerSetTowhookAttachState)

public:
    // The towhook has hit Component at WorldLocation (WorldNormal): attach the vehicle there.
    void SetTowhookAttachState(const TWeakObjectPtr<UPrimitiveComponent>& Component, const FVector& WorldLocation, const FVector& WorldNormal);
    void ServerSetTowhookAttachState(const FNetTowhookAttachState& InTowhookAttachState);

    static void HookCallSites();

private:
    static void SetTowhookAttachStateCall(AFortOctopusVehicle* This, const TWeakObjectPtr<UPrimitiveComponent>& Component, const FVector& WorldLocation, const FVector& WorldNormal);
};
