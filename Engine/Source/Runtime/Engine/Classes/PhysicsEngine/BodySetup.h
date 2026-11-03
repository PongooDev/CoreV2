#pragma once

#include "Math/Rotator.h"
#include "Math/Vector.h"
#include "Reflection.h"
#include "StructArray.h"
#include "UObject/Object.h"

class UPrimitiveComponent;
struct FTransform;

// A box collision shape of a body setup (FKShapeElem: it has a vtable, so it is only made through its script struct).
struct FKBoxElem
{
    DefineUnrealStruct(FKBoxElem, Engine)

public:
    DefineUProperty(FVector, Center)
    DefineUProperty(FRotator, Rotation)
    DefineUProperty(float, X)
    DefineUProperty(float, Y)
    DefineUProperty(float, Z)
};

// A body setup's simple collision shapes.
struct FKAggregateGeom
{
    DefineUnrealStruct(FKAggregateGeom, Engine)

public:
    DefineUStructArrayProperty(FKBoxElem, BoxElems)
};

// The physics state of one rigid body (UPrimitiveComponent::BodyInstance and the like).
struct FBodyInstance
{
    DefineUnrealStruct(FBodyInstance, Engine)

public:
    // FBodyInstance::InitBody: creates the body's physics actor at Transform from Setup's shapes. A null InRBScene keeps
    // it out of every physics scene, for overlap tests only (AFortPoiVolume::PostInitializeComponents does so). Null
    // when the build has no caller Core finds it through.
    void InitBody(class UBodySetup* Setup, const FTransform& Transform, UPrimitiveComponent* PrimComp, void* InRBScene);
};

// The collision shapes a component's body is made from.
class UBodySetup : public UObject
{
    DefineUnrealClass(UBodySetup, Engine)

public:
    DefineUStructProperty(FKAggregateGeom, AggGeom)
};
