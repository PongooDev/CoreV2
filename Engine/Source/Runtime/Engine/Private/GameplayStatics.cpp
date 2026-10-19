#include "pch.h"
#include "Kismet/GameplayStatics.h"
#include "ReflectionParams.h"

AActor* UGameplayStatics::BeginDeferredActorSpawnFromClass(const UObject* WorldContextObject, UClass* ActorClass, const FTransform& SpawnTransform, ESpawnActorCollisionHandlingMethod CollisionHandlingOverride, AActor* Owner)
{
    Reflection::FParams Params(StaticFunction_BeginDeferredActorSpawnFromClass());
    Params.Set(L"WorldContextObject", WorldContextObject);
    Params.Set(L"ActorClass", ActorClass);
    Params.Set(L"SpawnTransform", SpawnTransform);
    Params.Set(L"CollisionHandlingOverride", static_cast<uint8_t>(CollisionHandlingOverride));
    Params.Set(L"Owner", Owner);
    return Params.Call(StaticClass()->GetDefaultObject()) ? Params.Take<AActor*>(L"ReturnValue") : nullptr;
}

AActor* UGameplayStatics::FinishSpawningActor(AActor* Actor, const FTransform& SpawnTransform)
{
    Reflection::FParams Params(StaticFunction_FinishSpawningActor());
    Params.Set(L"Actor", Actor);
    Params.Set(L"SpawnTransform", SpawnTransform);
    return Params.Call(StaticClass()->GetDefaultObject()) ? Params.Take<AActor*>(L"ReturnValue") : nullptr;
}
