#pragma once

#include "Containers/Array.h"
#include "Engine/EngineTypes.h"
#include "Math/Transform.h"
#include "UObject/Object.h"

class AActor;

class UGameplayStatics : public UObject
{
    DefineUnrealClass(UGameplayStatics, Engine)
    DefineUFunction(BeginDeferredActorSpawnFromClass)
    DefineUFunction(FinishSpawningActor)
    DefineUFunction(GetAllActorsOfClass)

public:
    static AActor* BeginDeferredActorSpawnFromClass(const UObject* WorldContextObject, UClass* ActorClass, const FTransform& SpawnTransform, ESpawnActorCollisionHandlingMethod CollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::Undefined, AActor* Owner = nullptr);
    static AActor* FinishSpawningActor(AActor* Actor, const FTransform& SpawnTransform);
    // Every actor of the class (or a subclass) in the world; empty when the build has no GetAllActorsOfClass.
    static TArray<AActor*> GetAllActorsOfClass(const UObject* WorldContextObject, UClass* ActorClass);
};
