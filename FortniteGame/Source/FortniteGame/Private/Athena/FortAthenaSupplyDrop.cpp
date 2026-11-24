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

#include <algorithm>
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

AFortPickup* AFortAthenaSupplyDrop::SpawnPickupFromItemEntry(UFortWorldItemDefinition* ItemDefinition, const FFortItemEntry& InItemEntry, int32_t NumberToSpawn, AFortPawn* TriggeringPawn, const FVector& Position, const FVector& Direction)
{
#if WITH_SERVER_CODE
    UWorld* World = GetWorld();
    if (!World || !ItemDefinition || NumberToSpawn <= 0)
    {
        return nullptr;
    }

    // The rolled entry as it is (its level from the loot level), NumberToSpawn of it, for ItemDefinition.
    UScriptStruct* EntryStruct = FFortItemEntry::StaticStruct();
    std::vector<uint8_t> EntryStorage(EntryStruct->GetStructureSize(), 0);
    FFortItemEntry* ItemEntry = reinterpret_cast<FFortItemEntry*>(EntryStorage.data());
    EntryStruct->InitializeStruct(ItemEntry);
    EntryStruct->CopyScriptStruct(ItemEntry, &InItemEntry);
    ItemEntry->ItemDefinition = ItemDefinition;
    ItemEntry->Count = NumberToSpawn;

    const FRotator SpawnRotation = Direction.Rotation();
    AFortPickup* Pickup = AFortPickup::CreatePickup(World, *ItemEntry, &Position, &SpawnRotation);
    EntryStruct->DestroyStruct(ItemEntry);

    // The pawn that opened the drop is the toss's item owner, as SpawnPickup's.
    if (Pickup)
    {
        Pickup->TossPickup(Position, TriggeringPawn, 0);
    }
    return Pickup;
#else
    return nullptr;
#endif
}

AFortPickup* AFortAthenaSupplyDrop::SpawnGameModePickup(UFortWorldItemDefinition* ItemDefinition, UClass* PickupClass, int32_t NumberToSpawn, AFortPawn* TriggeringPawn, const FVector& Position, const FVector& Direction)
{
#if WITH_SERVER_CODE
    UWorld* World = GetWorld();
    UClass* GameModePickupClass = FindObject<UClass>(nullptr, L"/Script/FortniteGame.FortGameModePickup");
    if (!World || !ItemDefinition || NumberToSpawn <= 0 || !GameModePickupClass)
    {
        return nullptr;
    }
    // As the build's remnant (0xF83450) keeps it: PickupClass when it is an AFortGameModePickup, else that class.
    UClass* SpawnClass = PickupClass && PickupClass->IsChildOf(GameModePickupClass) ? PickupClass : GameModePickupClass;

    UScriptStruct* EntryStruct = FFortItemEntry::StaticStruct();
    TArray<uint8_t> EntryStorage;
    EntryStorage.AddZeroed(EntryStruct->GetStructureSize());
    FFortItemEntry* ItemEntry = reinterpret_cast<FFortItemEntry*>(EntryStorage.GetData());
    FFortItemEntry::Construct(ItemEntry, ItemDefinition, NumberToSpawn, 0);

    const FRotator SpawnRotation = Direction.Rotation();
    AFortPickup* Pickup = AFortPickup::CreatePickup(World, *ItemEntry, &Position, &SpawnRotation, nullptr, SpawnClass);
    EntryStruct->DestroyStruct(ItemEntry);
    // Core's: tossed as SpawnPickup's are, the pawn that opened the drop its item owner.
    if (Pickup)
    {
        Pickup->TossPickup(Position, TriggeringPawn, 0);
        Log::Info(L"Supply drop: {} lands {} {} as {}", GetName().ToStdString(), NumberToSpawn, ItemDefinition->GetName().ToStdString(), Pickup->GetName().ToStdString());
    }
    return Pickup;
#else
    return nullptr;
#endif
}

DEFINE_FUNCTION(AFortAthenaSupplyDrop::execSpawnGameModePickup)
{
    P_GET_OBJECT(UFortWorldItemDefinition, Z_Param_ItemDefinition);
    P_GET_OBJECT(UClass, Z_Param_PickupClass);
    P_GET_PROPERTY(UIntProperty, Z_Param_NumberToSpawn);
    P_GET_OBJECT(AFortPawn, Z_Param_TriggeringPawn);
    P_GET_STRUCT(FVector, Z_Param_Position);
    P_GET_STRUCT(FVector, Z_Param_Direction);
    P_FINISH;
    P_NATIVE_BEGIN;
    *static_cast<AFortPickup**>(RESULT_PARAM) = static_cast<AFortAthenaSupplyDrop*>(Context)->SpawnGameModePickup(Z_Param_ItemDefinition, Z_Param_PickupClass, Z_Param_NumberToSpawn, Z_Param_TriggeringPawn,
        Z_Param_Position, Z_Param_Direction);
    P_NATIVE_END;
}

DEFINE_FUNCTION(AFortAthenaSupplyDrop::execSpawnPickupFromItemEntry)
{
    // InItemEntry is the build's FFortItemEntry (its size from reflection), stepped into storage of its own.
    UScriptStruct* EntryStruct = FFortItemEntry::StaticStruct();
    std::vector<uint8_t> EntryStorage(EntryStruct->GetStructureSize(), 0);
    EntryStruct->InitializeStruct(EntryStorage.data());
    P_GET_OBJECT(UFortWorldItemDefinition, Z_Param_ItemDefinition);
    P_STACK.StepCompiledIn<UStructProperty>(EntryStorage.data());
    P_GET_PROPERTY(UIntProperty, Z_Param_NumberToSpawn);
    P_GET_OBJECT(AFortPawn, Z_Param_TriggeringPawn);
    P_GET_STRUCT(FVector, Z_Param_Position);
    P_GET_STRUCT(FVector, Z_Param_Direction);
    P_FINISH;
    P_NATIVE_BEGIN;
    *static_cast<AFortPickup**>(RESULT_PARAM) = static_cast<AFortAthenaSupplyDrop*>(Context)->SpawnPickupFromItemEntry(Z_Param_ItemDefinition, *reinterpret_cast<const FFortItemEntry*>(EntryStorage.data()), Z_Param_NumberToSpawn, Z_Param_TriggeringPawn, Z_Param_Position, Z_Param_Direction);
    P_NATIVE_END;
    EntryStruct->DestroyStruct(EntryStorage.data());
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
    if (!Natives::FFrame_Step && !Finder::ResolveScriptFrame())
    {
        return;
    }
    HookSpawnPickup();
    HookSpawnPickupFromItemEntry();
    HookSpawnGameModePickup();
}

void AFortAthenaSupplyDrop::HookSpawnPickup()
{
    // SpawnPickup is 1.10's (CoreV2 #170). 1.10's exec (0x9FE050) steps its five parameters and calls the shared
    // return-zero stub (UserMathErrorFunction, 0x1AC45D0) instead of the native, so every pickup the crate's SpawnTierDrops
    // asked for was null and the crate opened onto nothing. 1.9.1 and earlier have no SpawnPickup (their crate calls
    // K2_SpawnPickupInWorld), so it is looked up without failing there. 8.20's exec (0x17D1BE0) calls a stripped body
    // that keeps its AFortPickupAthena::StaticClass() call and returns null (0xE4C0F0), so Battle Royale's supply drops
    // opened onto nothing (#555).
    UFunction* Function = FindStaticFunction_SpawnPickup();
    if (!Function)
    {
        return;
    }
    uint8_t* NativeCall = Finder::ImplementationCall(Function);
    const uintptr_t Native = NativeCall ? Finder::RelativeTarget(reinterpret_cast<uintptr_t>(NativeCall), 1, 5) : 0;
    if (!Native || (!Finder::IsStubFunction(Native) && !Finder::IsCallThenReturnZeroStub(Native)))
    {
        Log::Info(L"Finder: AFortAthenaSupplyDrop::SpawnPickup is native in this build");
        return;
    }
    Function->SetNativeFunc(&execSpawnPickup);
    Log::Info(L"Hook AFortAthenaSupplyDrop::SpawnPickup: native");
}

void AFortAthenaSupplyDrop::HookSpawnPickupFromItemEntry()
{
    // SpawnPickupFromItemEntry is 7.x's. 7.10's exec (0x1533290) steps its six parameters and calls the shared return-zero
    // stub (UserMathErrorFunction, 0x2A5340) instead of the native, so every pickup Frostnite's supply drops
    // (EndlessModeSupplyDrop_C's SpawnLoot) and its BluGlo drop asked for was null: they opened onto nothing (#491).
    UFunction* Function = FindStaticFunction_SpawnPickupFromItemEntry();
    if (!Function)
    {
        return;
    }
    uint8_t* NativeCall = Finder::ImplementationCall(Function);
    if (!NativeCall || !Finder::IsStubFunction(Finder::RelativeTarget(reinterpret_cast<uintptr_t>(NativeCall), 1, 5)))
    {
        Log::Info(L"Finder: AFortAthenaSupplyDrop::SpawnPickupFromItemEntry is native in this build");
        return;
    }
    Function->SetNativeFunc(&execSpawnPickupFromItemEntry);
    Log::Info(L"Hook AFortAthenaSupplyDrop::SpawnPickupFromItemEntry: native");
}

void AFortAthenaSupplyDrop::HookSpawnGameModePickup()
{
    // 8.50 (CL-6058028): the exec (0x1978CE0) steps the six parameters and calls a remnant (0xF83450) that only checks
    // PickupClass against AFortGameModePickup::StaticClass() and returns null, so Endgame's stone supply drops landed no
    // stone (CoreV2 #630). A real body spawns the pickup: the remnant is small and calls only one function (StaticClass).
    UFunction* Function = FindStaticFunction_SpawnGameModePickup();
    if (!Function)
    {
        return;
    }
    uint8_t* NativeCall = Finder::ImplementationCall(Function);
    const uintptr_t Native = NativeCall ? Finder::RelativeTarget(reinterpret_cast<uintptr_t>(NativeCall), 1, 5) : 0;
    const uintptr_t NativeEnd = Native && Finder::FunctionStart(Native) == Native ? Finder::FunctionEnd(Native) : 0;
    const std::vector<std::pair<uintptr_t, uintptr_t>> Calls = NativeEnd ? Finder::DecodedDirectCalls(Native) : std::vector<std::pair<uintptr_t, uintptr_t>>{};
    const bool bOneCallee = std::all_of(Calls.begin(), Calls.end(), [&Calls](const std::pair<uintptr_t, uintptr_t>& Call) { return Call.second == Calls.front().second; });
    if (!NativeEnd || (!Finder::IsStubFunction(Native) && (NativeEnd - Native > 0x100 || !bOneCallee)))
    {
        Log::Info(L"Finder: AFortAthenaSupplyDrop::SpawnGameModePickup is native in this build");
        return;
    }
    Function->SetNativeFunc(&execSpawnGameModePickup);
    Log::Info(L"Hook AFortAthenaSupplyDrop::SpawnGameModePickup: native (the build's 0x{:X} returns null)", Native - Finder::ImageBase());
}
