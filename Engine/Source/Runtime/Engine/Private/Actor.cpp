#include "pch.h"
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
