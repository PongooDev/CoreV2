#include "pch.h"
#include "Athena/FortOctopusVehicle.h"
#include "Components/PrimitiveComponent.h"
#include "Finder.h"
#include "Hooks.h"
#include "Log.h"
#include "Math/Transform.h"
#include "Natives.h"
#include "ReflectionParams.h"

namespace
{
    // FTowhookAttachState keeps the point in the component's body space: the world getter (8.20 CL-5625478 0xF57770)
    // takes Component->GetBodyInstance(NAME_None, true) and its GetUnrealWorldTransform(true, false), and gives nothing
    // for a component without a body.
    bool GetBodyWorldTransform(UPrimitiveComponent* Component, FTransform& OutTransform)
    {
        if (!Component || Natives::UPrimitiveComponent_GetBodyInstanceIndex < 0 || !Natives::FBodyInstance_GetUnrealWorldTransform)
        {
            return false;
        }
        using FGetBodyInstance = void* (*)(UPrimitiveComponent* This, uint64_t BoneName, bool bGetWelded);
        const auto GetBodyInstance = reinterpret_cast<FGetBodyInstance>((*reinterpret_cast<void***>(Component))[Natives::UPrimitiveComponent_GetBodyInstanceIndex]);
        void* BodyInstance = GetBodyInstance(Component, 0, true);
        if (!BodyInstance)
        {
            return false;
        }
        Natives::FBodyInstance_GetUnrealWorldTransform(BodyInstance, &OutTransform, true, false);
        return true;
    }
}

// The Baller's grapple let go at full length (#564). When the towhook hits, AFortAttachableProjectileBase::
// ServerAttachToActor_Implementation attaches the projectile and calls OnRep_ReplicatedAttachedInfo, whose
// AFortOctopusTowhookAttachableProjectile override hands the hit to its OwningVehicle here (8.20 0x1723E7C, 8.00
// 0x161C3BD). The client builds compile this out: the call goes into the shared empty function, and the UFunction
// SetTowhookAttachState's native (8.20 0xEB25F0) only makes the TWeakObjectPtr. Nothing then sets the vehicle's attach
// state, so its towhook model (the vehicle's tick, 8.20 0xEBA520) never attaches and retracts once the projectile is out
// of range. The state is set the way the game's own ServerSetTowhookAttachState_Implementation does it (8.20 0xEB1470:
// ReplicatedAttachState and the cached world copy the tick reads), from the point in the hit component's body space.
void AFortOctopusVehicle::SetTowhookAttachState(const TWeakObjectPtr<UPrimitiveComponent>& Component, const FVector& WorldLocation, const FVector& WorldNormal)
{
#if WITH_SERVER_CODE
    FNetTowhookAttachState NewState;
    NewState.Component = Component.Get();
    FTransform BodyTransform;
    if (GetBodyWorldTransform(NewState.Component, BodyTransform))
    {
        NewState.LocalLocation = BodyTransform.InverseTransformPosition(WorldLocation);
        NewState.LocalNormal = BodyTransform.InverseTransformVectorNoScale(WorldNormal);
    }
    ServerSetTowhookAttachState(NewState);
#endif
}

// On the server the RPC runs its _Implementation (its _Validate, 8.20 0x2C3B40, accepts every state).
void AFortOctopusVehicle::ServerSetTowhookAttachState(const FNetTowhookAttachState& InTowhookAttachState)
{
    Reflection::FParams Params(StaticFunction_ServerSetTowhookAttachState());
    Params.Set(L"InTowhookAttachState", InTowhookAttachState);
    Params.Call(this);
}

void AFortOctopusVehicle::HookCallSites()
{
    // 8.00+: the Baller's classes and its RPC (7.20 to 7.40 have an AFortOctopusVehicle, the finder logs what they lack).
    if (!StaticClass() || !FindStaticFunction_ServerSetTowhookAttachState() || !FNetTowhookAttachState::StaticStruct())
    {
        return;
    }
    if (FNetTowhookAttachState::StaticStruct()->GetPropertiesSize() != sizeof(FNetTowhookAttachState))
    {
        Log::Error(L"Finder: FNetTowhookAttachState is 0x{:X} bytes, not 0x{:X}; the Baller's grapple attach is not restored",
            FNetTowhookAttachState::StaticStruct()->GetPropertiesSize(), sizeof(FNetTowhookAttachState));
        return;
    }
    if (Finder::ResolveOctopusTowhookLive())
    {
        Hooks::RedirectCall(L"AFortOctopusTowhookAttachableProjectile::OnRep_ReplicatedAttachedInfo -> AFortOctopusVehicle::SetTowhookAttachState",
            Natives::AFortOctopusTowhookAttachableProjectile_OnRep_ReplicatedAttachedInfo_SetTowhookAttachStateCall, reinterpret_cast<void*>(&SetTowhookAttachStateCall));
    }
}

void AFortOctopusVehicle::SetTowhookAttachStateCall(AFortOctopusVehicle* This, const TWeakObjectPtr<UPrimitiveComponent>& Component, const FVector& WorldLocation, const FVector& WorldNormal)
{
    This->SetTowhookAttachState(Component, WorldLocation, WorldNormal);
}
