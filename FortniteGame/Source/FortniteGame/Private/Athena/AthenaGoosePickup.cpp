#include "pch.h"
#include "Athena/AthenaGoosePickup.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "Logging/LogMacros.h"
#include "Math/Transform.h"
#include "ReflectionParams.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"

DEFINE_LOG_CATEGORY_STATIC(LogGoosePickup, Log, All);

namespace
{
    // The constructor's height over the ground (8.40 0xE89120: mov dword ptr [rbx+8ECh], 447A0000h).
    constexpr float ConstructorDeployAltitude = 1000.0f;

    const uint8_t* PickupDefault()
    {
        UClass* Class = AAthenaGoosePickup::StaticClass();
        return Class ? reinterpret_cast<const uint8_t*>(Class->GetDefaultObject(false)) : nullptr;
    }
}

ptrdiff_t AAthenaGoosePickup::LootTableNameOffset()
{
    static ptrdiff_t Offset = -2;
    if (Offset == -2)
    {
        Offset = -1;
        const uint8_t* Default = HasSpawnMembers() ? PickupDefault() : nullptr;
        const ptrdiff_t Candidate = Default ? Member_DisplayIndex.GetOffsetChecked() + static_cast<ptrdiff_t>(sizeof(int32_t)) : -1;
        uint64_t Bits = ~0ull;
        if (Candidate > 0)
        {
            std::memcpy(&Bits, Default + Candidate, sizeof(Bits));
        }
        if (Bits == 0)
        {
            Offset = Candidate;
        }
    }
    return Offset;
}

ptrdiff_t AAthenaGoosePickup::DeployAltitudeOffset()
{
    static ptrdiff_t Offset = -2;
    if (Offset == -2)
    {
        Offset = -1;
        const uint8_t* Default = HasSpawnMembers() ? PickupDefault() : nullptr;
        const ptrdiff_t Candidate = Default ? Member_DeployHeight.GetOffsetChecked() + static_cast<ptrdiff_t>(sizeof(float)) : -1;
        float Value = 0.0f;
        if (Candidate > 0)
        {
            std::memcpy(&Value, Default + Candidate, sizeof(Value));
        }
        if (Value == ConstructorDeployAltitude)
        {
            Offset = Candidate;
        }
    }
    return Offset;
}

AAthenaGoosePickup* AAthenaGoosePickup::SpawnGoosePickup(UWorld* World, const FGoosePickupSpawnInfo& Info)
{
#if WITH_SERVER_CODE
    const ptrdiff_t LootOffset = LootTableNameOffset();
    const ptrdiff_t AltitudeOffset = DeployAltitudeOffset();
    if (!World || !Info.PickupClass || LootOffset < 0 || AltitudeOffset < 0)
    {
        return nullptr;
    }

    FTransform SpawnTransform;
    SpawnTransform.Rotation = Info.Rotation.Quaternion();
    SpawnTransform.SetLocation(Info.Location);
    AAthenaGoosePickup* Pickup = Cast<AAthenaGoosePickup>(UGameplayStatics::BeginDeferredActorSpawnFromClass(World, Info.PickupClass, SpawnTransform, ESpawnActorCollisionHandlingMethod::AlwaysSpawn));
    if (!Pickup)
    {
        UE_LOG(LogGoosePickup, Error, TEXT("SpawnGoosePickup: couldn't spawn %s"), *Info.PickupClass->GetName());
        return nullptr;
    }

    const FName Forced = Pickup->ForcedLootTableName;
    const FName LootTableName = Forced.IsNone() ? Info.GetLootTableName() : Forced;
    uint8_t* const PickupBytes = reinterpret_cast<uint8_t*>(Pickup);
    std::memcpy(PickupBytes + LootOffset, &LootTableName, sizeof(LootTableName));
    std::memcpy(PickupBytes + AltitudeOffset, &Info.DeployAltitude, sizeof(Info.DeployAltitude));

    int32_t Display = -1;
    TStructArray<FGooseDisplayData>& Displays = Pickup->DisplayData;
    for (int32_t Index = 0; Index < Displays.Num() && Display < 0; ++Index)
    {
        if (static_cast<FName>(Displays[Index].LootTableName) == LootTableName)
        {
            Display = Index;
        }
    }
    Pickup->DisplayIndex = Display;
    UGameplayStatics::FinishSpawningActor(Pickup, SpawnTransform);
    // The server's copy gets the look too (its OnRep only runs on clients).
    if (Display >= 0)
    {
        Pickup->OnRep_DisplayIndex();
    }

    UE_LOG(LogGoosePickup, Log, TEXT("SpawnGoosePickup: %s with %s at (%.0f, %.0f, %.0f), it stops %.0f over the ground"), *Pickup->GetName(), *LootTableName.ToString(),
        Info.Location.X, Info.Location.Y, Info.Location.Z, Info.DeployAltitude);
    return Pickup;
#else
    return nullptr;
#endif
}

void AAthenaGoosePickup::OnRep_DisplayIndex()
{
    Reflection::FParams Params(StaticFunction_OnRep_DisplayIndex());
    Params.Call(this);
}
