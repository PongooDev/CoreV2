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

    int32_t GetNumMaterials() const;
    UMaterialInterface* GetMaterial(int32_t ElementIndex) const;
    void SetMaterial(int32_t ElementIndex, UMaterialInterface* Material);
};

class UMeshComponent : public UPrimitiveComponent
{
    DefineUnrealClass(UMeshComponent, Engine)
};

class UStaticMeshComponent : public UMeshComponent
{
    DefineUnrealClass(UStaticMeshComponent, Engine)

public:
    DefineUFunction(GetLocalBounds)

    // The mesh's box in the component's own space. False if there is no mesh or no GetLocalBounds.
    bool GetLocalBounds(FVector& Min, FVector& Max) const;
};
