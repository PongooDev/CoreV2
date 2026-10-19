#pragma once

#include "Engine/EngineTypes.h"
#include "Math/Transform.h"
#include "UObject/Object.h"

class AActor;

class UGameplayStatics : public UObject
{
    DefineUnrealClass(UGameplayStatics, Engine)
    DefineUFunction(BeginDeferredActorSpawnFromClass)
    DefineUFunction(FinishSpawningActor)

public:
    static AActor* BeginDeferredActorSpawnFromClass(const UObject* WorldContextObject, UClass* ActorClass, const FTransform& SpawnTransform, ESpawnActorCollisionHandlingMethod CollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::Undefined, AActor* Owner = nullptr);
    static AActor* FinishSpawningActor(AActor* Actor, const FTransform& SpawnTransform);
};
