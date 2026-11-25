#pragma once

#include "GameFramework/Actor.h"
#include "Reflection.h"

class UStaticMeshComponent;

class AStaticMeshActor : public AActor
{
    DefineUnrealClass(AStaticMeshActor, Engine)

public:
    DefineUProperty(UStaticMeshComponent*, StaticMeshComponent)
};
