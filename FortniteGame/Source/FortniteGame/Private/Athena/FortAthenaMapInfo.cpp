#include "pch.h"
#include "Athena/FortAthenaMapInfo.h"
#include "Finder.h"
#include "Math/Rotator.h"
#include "Math/Vector.h"
#include "Natives.h"

bool AFortAthenaMapInfo::HasLlamas()
{
    return Member_LlamaClass.IsAvailable() && Member_LlamaQuantityMin.IsAvailable() && Member_LlamaQuantityMax.IsAvailable()
        && Finder::ResolveSupplyDropSpawnLive(Member_LlamaClass.GetOffsetChecked());
}

FVector AFortAthenaMapInfo::PickSupplyDropLocation(const FVector& Center, float Radius)
{
    FVector Result = Center;
    if (Natives::AFortAthenaMapInfo_PickSupplyDropLocation)
    {
        Natives::AFortAthenaMapInfo_PickSupplyDropLocation(this, &Result, Center, Radius);
    }
    return Result;
}

AFortAthenaSupplyDrop* AFortAthenaMapInfo::SpawnSupplyDropAtLocation(const FVector& Location, const FRotator& Rotation, UClass* SupplyDropClass)
{
    return Natives::AFortAthenaMapInfo_SpawnSupplyDropAtLocation ? Natives::AFortAthenaMapInfo_SpawnSupplyDropAtLocation(this, Location, Rotation, SupplyDropClass) : nullptr;
}
