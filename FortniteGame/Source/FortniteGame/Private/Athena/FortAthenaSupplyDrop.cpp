#include "pch.h"
#include "Athena/FortAthenaSupplyDrop.h"
#include "Engine/World.h"
#include "Finder.h"
#include "Items/FortItemDefinition.h"
#include "Items/FortPickup.h"
#include "Log.h"
#include "Natives.h"
#include "Math/Rotator.h"
#include "Math/Vector.h"
#include "Pawns/FortPawn.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

#include <vector>

AFortPickup* AFortAthenaSupplyDrop::SpawnPickup(UFortWorldItemDefinition* ItemDefinition, int32_t NumberToSpawn, AFortPawn* TriggeringPawn, const FVector& Position, const FVector& Direction)
{
#if WITH_SERVER_CODE
    UWorld* World = GetWorld();
    if (!World || !ItemDefinition || NumberToSpawn <= 0)
    {
        return nullptr;
    }

    UScriptStruct* EntryStruct = FFortItemEntry::StaticStruct();
    std::vector<uint8_t> EntryStorage(EntryStruct->GetStructureSize(), 0);
    FFortItemEntry* ItemEntry = reinterpret_cast<FFortItemEntry*>(EntryStorage.data());
    FFortItemEntry::Construct(ItemEntry, ItemDefinition, NumberToSpawn, 0);
    // A supply drop's weapons come loaded (#85), as they did through K2_SpawnPickupInWorld before 1.10.
    ItemEntry->LoadFullClip();

    const FRotator SpawnRotation = Direction.Rotation();
    AFortPickup* Pickup = AFortPickup::CreatePickup(World, *ItemEntry, &Position, &SpawnRotation);
    EntryStruct->DestroyStruct(ItemEntry);

    // The pawn that searched the crate is the toss's item owner.
    if (Pickup)
    {
        Pickup->TossPickup(Position, TriggeringPawn, 0);
    }
    return Pickup;
#else
    return nullptr;
#endif
}

DEFINE_FUNCTION(AFortAthenaSupplyDrop::execSpawnPickup)
{
    P_GET_OBJECT(UFortWorldItemDefinition, Z_Param_ItemDefinition);
    P_GET_PROPERTY(UIntProperty, Z_Param_NumberToSpawn);
    P_GET_OBJECT(AFortPawn, Z_Param_TriggeringPawn);
    P_GET_STRUCT(FVector, Z_Param_Position);
    P_GET_STRUCT(FVector, Z_Param_Direction);
    P_FINISH;
    P_NATIVE_BEGIN;
    *static_cast<AFortPickup**>(RESULT_PARAM) = static_cast<AFortAthenaSupplyDrop*>(Context)->SpawnPickup(Z_Param_ItemDefinition, Z_Param_NumberToSpawn, Z_Param_TriggeringPawn, Z_Param_Position, Z_Param_Direction);
    P_NATIVE_END;
}

void AFortAthenaSupplyDrop::HookNatives()
{
    // SpawnPickup is 1.10's (CoreV2 #170). 1.10's exec (0x9FE050) steps its five parameters and calls the shared
    // return-zero stub (UserMathErrorFunction, 0x1AC45D0) instead of the native, so every pickup the crate's SpawnTierDrops
    // asked for was null and the crate opened onto nothing. 1.9.1 and earlier have no SpawnPickup (their crate calls
    // K2_SpawnPickupInWorld), so it is looked up without failing there.
    UFunction* Function = FindStaticFunction_SpawnPickup();
    if (!Function || (!Natives::FFrame_Step && !Finder::ResolveScriptFrame()))
    {
        return;
    }
    uint8_t* NativeCall = Finder::ImplementationCall(Function);
    if (!NativeCall || !Finder::IsStubFunction(Finder::RelativeTarget(reinterpret_cast<uintptr_t>(NativeCall), 1, 5)))
    {
        Log::Info(L"Finder: AFortAthenaSupplyDrop::SpawnPickup is native in this build");
        return;
    }
    Function->SetNativeFunc(&execSpawnPickup);
    Log::Info(L"Hook AFortAthenaSupplyDrop::SpawnPickup: native");
}
