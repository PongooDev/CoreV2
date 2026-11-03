#include "pch.h"
#include "PhysicsEngine/BodySetup.h"
#include "Math/Transform.h"
#include "Natives.h"

void FBodyInstance::InitBody(UBodySetup* Setup, const FTransform& Transform, UPrimitiveComponent* PrimComp, void* InRBScene)
{
    if (!Natives::FBodyInstance_InitBody || !Natives::FInitBodySpawnParams_Construct)
    {
        return;
    }
    // FInitBodySpawnParams: bStaticPhysics, bPhysicsTypeDeterminesSimulation, bDynamicActorScene? and the aggregate.
    alignas(8) uint8_t SpawnParams[0x10] = {};
    Natives::FInitBodySpawnParams_Construct(SpawnParams, PrimComp);
    Natives::FBodyInstance_InitBody(this, Setup, Transform, PrimComp, InRBScene, SpawnParams);
}
