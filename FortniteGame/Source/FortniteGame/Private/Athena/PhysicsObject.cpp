#include "pch.h"
#include "Athena/PhysicsObject.h"
#include "ReflectionParams.h"

void APhysicsObject::ApplyImpulseFromPawnHit(const FVector& VelocityOfHitter, const FVector& NormalTowardObject, APawn* HittingPawn)
{
    Reflection::FParams Params(FindStaticFunction_ApplyImpulseFromPawnHit());
    if (Params.Set<FVector>(L"VelocityOfHitter", VelocityOfHitter) && Params.Set<FVector>(L"NormalTowardObject", NormalTowardObject) && Params.Set<APawn*>(L"HittingPawn", HittingPawn))
    {
        Params.Call(this);
    }
}
