#pragma once

#include "Components/SceneComponent.h"
#include "Reflection.h"

class UMaterialInterface;

class UPrimitiveComponent : public USceneComponent
{
    DefineUnrealClass(UPrimitiveComponent, Engine)

public:
    DefineUFunction(GetNumMaterials)
    DefineUFunction(GetMaterial)
    DefineUFunction(SetMaterial)
    DefineUFunction(SetPhysicsLinearVelocity)
    DefineUFunction(SetPhysicsAngularVelocityInRadians)
    DefineUFunction(GetCollisionEnabled)

    int32_t GetNumMaterials() const;
    UMaterialInterface* GetMaterial(int32_t ElementIndex) const;
    void SetMaterial(int32_t ElementIndex, UMaterialInterface* Material);
    void SetPhysicsLinearVelocity(const FVector& NewVel, bool bAddToCurrent = false, FName BoneName = FName());
    void SetPhysicsAngularVelocityInRadians(const FVector& NewAngVel, bool bAddToCurrent = false, FName BoneName = FName());
    // ECollisionEnabled::Type (0 NoCollision). Callers gate on FindStaticFunction_GetCollisionEnabled.
    uint8_t GetCollisionEnabled() const;
};

class UStaticMesh;

class UMeshComponent : public UPrimitiveComponent
{
    DefineUnrealClass(UMeshComponent, Engine)
};

class UStaticMeshComponent : public UMeshComponent
{
    DefineUnrealClass(UStaticMeshComponent, Engine)

public:
    DefineUFunction(GetLocalBounds)
    DefineUProperty(UStaticMesh*, StaticMesh)

    // The mesh's box in the component's own space. False if there is no mesh or no GetLocalBounds.
    bool GetLocalBounds(FVector& Min, FVector& Max) const;
};
