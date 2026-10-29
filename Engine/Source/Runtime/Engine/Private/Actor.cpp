#include "pch.h"
#include "ReflectionParams.h"
#include "GameFramework/Actor.h"
#include "Engine/DemoNetDriver.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/NetDriver.h"
#include "Engine/World.h"
#include "Hooks.h"
#include "Natives.h"
#include "Templates/Casts.h"

ULevel* AActor::GetLevel() const
{
    return Cast<ULevel>(GetOuter());
}

UWorld* AActor::GetWorld() const
{
    if (!HasAnyFlags(RF_ClassDefaultObject) && !GetOuter()->HasAnyFlags(RF_BeginDestroyed) && !GetOuter()->IsUnreachable())
    {
        if (ULevel* Level = GetLevel())
        {
            return Level->OwningWorld;
        }
    }
    return nullptr;
}

FTransform AActor::GetTransform() const
{
    Reflection::FParams Params(StaticFunction_GetTransform());
    return Params.Call(const_cast<AActor*>(this)) ? Params.Take<FTransform>(L"ReturnValue") : FTransform();
}

void AActor::GetActorBounds(bool bOnlyCollidingComponents, FVector& Origin, FVector& BoxExtent) const
{
    Reflection::FParams Params(StaticFunction_GetActorBounds());
    Params.Set(L"bOnlyCollidingComponents", bOnlyCollidingComponents);
    const bool bCalled = Params.Call(const_cast<AActor*>(this));
    Origin = bCalled ? Params.Take<FVector>(L"Origin") : FVector(0, 0, 0);
    BoxExtent = bCalled ? Params.Take<FVector>(L"BoxExtent") : FVector(0, 0, 0);
}

TArray<UActorComponent*> AActor::GetComponentsByClass(UClass* ComponentClass) const
{
    Reflection::FParams Params(StaticFunction_GetComponentsByClass());
    Params.Set(L"ComponentClass", ComponentClass);
    return Params.Call(const_cast<AActor*>(this)) ? Params.Take<TArray<UActorComponent*>>(L"ReturnValue") : TArray<UActorComponent*>();
}

bool AActor::SetActorLocation(const FVector& NewLocation, bool bSweep, bool bTeleport)
{
    Reflection::FParams Params(StaticFunction_K2_SetActorLocation(), true);
    Params.Set(L"NewLocation", NewLocation);
    Params.Set(L"bSweep", bSweep);
    Params.Set(L"bTeleport", bTeleport);
    return Params.Call(this) && Params.Take<bool>(L"ReturnValue");
}

bool AActor::SetActorLocationAndRotation(const FVector& NewLocation, const FRotator& NewRotation, bool bSweep, bool bTeleport)
{
    Reflection::FParams Params(StaticFunction_K2_SetActorLocationAndRotation(), true);
    Params.Set(L"NewLocation", NewLocation);
    Params.Set(L"NewRotation", NewRotation);
    Params.Set(L"bSweep", bSweep);
    Params.Set(L"bTeleport", bTeleport);
    return Params.Call(this) && Params.Take<bool>(L"ReturnValue");
}

FRotator AActor::GetActorRotation() const
{
    Reflection::FParams Params(StaticFunction_K2_GetActorRotation());
    return Params.Call(const_cast<AActor*>(this)) ? Params.Take<FRotator>(L"ReturnValue") : FRotator();
}

bool AActor::TeleportTo(const FVector& DestLocation, const FRotator& DestRotation)
{
    Reflection::FParams Params(StaticFunction_K2_TeleportTo());
    Params.Set(L"DestLocation", DestLocation);
    Params.Set(L"DestRotation", DestRotation);
    return Params.Call(this) && Params.Take<bool>(L"ReturnValue");
}

void AActor::SetActorScale3D(const FVector& NewScale3D)
{
    Reflection::FParams Params(StaticFunction_SetActorScale3D());
    Params.Set(L"NewScale3D", NewScale3D);
    Params.Call(this);
}

FVector AActor::GetActorScale3D() const
{
    Reflection::FParams Params(StaticFunction_GetActorScale3D());
    return Params.Call(const_cast<AActor*>(this)) ? Params.Take<FVector>(L"ReturnValue") : FVector(1.0f, 1.0f, 1.0f);
}

void AActor::SetReplicates(bool bInReplicates)
{
    Reflection::FParams Params(StaticFunction_SetReplicates());
    Params.Set(L"bInReplicates", bInReplicates);
    Params.Call(this);
}

void AActor::SetActorEnableCollision(bool bNewActorEnableCollision)
{
    Reflection::FParams Params(StaticFunction_SetActorEnableCollision());
    Params.Set(L"bNewActorEnableCollision", bNewActorEnableCollision);
    Params.Call(this);
}

void AActor::SetActorHiddenInGame(bool bNewHidden)
{
    Reflection::FParams Params(StaticFunction_SetActorHiddenInGame());
    Params.Set(L"bNewHidden", bNewHidden);
    Params.Call(this);
}

void AActor::AttachToActor(AActor* Parent, EAttachmentRule LocationRule, EAttachmentRule RotationRule, EAttachmentRule ScaleRule)
{
    Reflection::FParams Params(StaticFunction_K2_AttachToActor(), true);
    Params.Set(L"ParentActor", Parent);
    Params.Set(L"SocketName", FName());
    Params.Set(L"LocationRule", LocationRule);
    Params.Set(L"RotationRule", RotationRule);
    Params.Set(L"ScaleRule", ScaleRule);
    Params.Set(L"bWeldSimulatedBodies", false);
    Params.Call(this);
}

void AActor::K2_DestroyActor()
{
    Reflection::FParams Params(StaticFunction_K2_DestroyActor());
    Params.Call(this);
}

FVector AActor::GetVelocity() const
{
    FVector Velocity;
    CallVirtual<FVector*>(this, Natives::AActor_GetVelocityIndex, &Velocity);
    return Velocity;
}

void AActor::CallPreReplication(UNetDriver* NetDriver)
{
    Natives::AActor_CallPreReplication(this, NetDriver);
}

bool AActor::IsRelevancyOwnerFor(const AActor* ReplicatedActor, const AActor* ActorOwner, const AActor* ConnectionActor) const
{
    return CallVirtual<bool>(this, Natives::AActor_IsRelevancyOwnerForIndex, ReplicatedActor, ActorOwner, ConnectionActor);
}

void AActor::ForceNetUpdate()
{
    CallVirtual<void>(this, Natives::AActor_ForceNetUpdateIndex);
}

void AActor::SetNetDormancy(ENetDormancy NewDormancy)
{
    Natives::AActor_SetNetDormancy(this, NewDormancy);
}

void AActor::FlushNetDormancy()
{
    Natives::AActor_FlushNetDormancy(this);
}

static UNetDriver* GetNetDriver_Internal(UWorld* World, FName NetDriverName)
{
    if (NetDriverName == NAME_GameNetDriver)
    {
        return World ? World->GetNetDriver() : nullptr;
    }

    return GEngine->FindNamedNetDriver(World, NetDriverName);
}

ENetMode AActor::InternalGetNetMode() const
{
    UWorld* World = GetWorld();
    if (UNetDriver* NetDriver = GetNetDriver_Internal(World, NetDriverName))
    {
        return NetDriver->GetNetMode();
    }

    if (World && World->DemoNetDriver)
    {
        return World->DemoNetDriver->GetNetMode();
    }

    return NM_Standalone;
}

UNetDriver* AActor::GetNetDriver() const
{
    return GetNetDriver_Internal(GetWorld(), NetDriverName);
}

void AActor::ReceiveTick(float DeltaSeconds)
{
    Natives::AActor_ReceiveTick(this, DeltaSeconds);
}

void AActor::Hook()
{
    if (Sites::ActorTickReceiveTick)
    {
        Hooks::RedirectCall(L"AActor::Tick IsRunningDedicatedServer", Sites::ActorTickReceiveTick, reinterpret_cast<void*>(&TickReceiveTickHK));
    }
}

void AActor::TickReceiveTickHK(AActor* This, float DeltaSeconds)
{
    if (This->bAllowReceiveTickEventOnDedicatedServer || !IsRunningDedicatedServer())
    {
        This->ReceiveTick(DeltaSeconds);
    }
}
