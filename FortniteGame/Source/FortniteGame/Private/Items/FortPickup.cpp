#include "pch.h"
#include "Items/FortPickup.h"
#include "Finder.h"
#include "Natives.h"
#include "ReflectionParams.h"

AFortPickup* AFortPickup::CreatePickup(UWorld* World, const FFortItemEntry& ItemEntry, const FVector* SpawnLocation, const FRotator* SpawnRotation, AFortPlayerController* OptionalOwner, UClass* OverrideClass)
{
    if (!Natives::AFortPickup_CreatePickup && !Finder::ResolvePickupLive())
    {
        return nullptr;
    }
    return Natives::AFortPickup_CreatePickup(World, ItemEntry, SpawnLocation, SpawnRotation, OptionalOwner, OverrideClass);
}

void AFortPickup::TossPickup(const FVector& FinalLocation, AFortPawn* ItemOwner, int32_t OverrideMaxStackCount)
{
    Reflection::FParams Params(StaticFunction_TossPickup());
    Params.Set(L"FinalLocation", FinalLocation);
    Params.Set(L"ItemOwner", ItemOwner);
    Params.Set(L"OverrideMaxStackCount", OverrideMaxStackCount);
    Params.Call(this);
}
