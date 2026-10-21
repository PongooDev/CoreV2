#include "pch.h"
#include "Missions/FortMissionLibrary.h"
#include "Engine/World.h"
#include "Finder.h"
#include "FortKismetLibrary.h"
#include "Items/FortPickup.h"
#include "Log.h"
#include "Natives.h"
#include "Pawns/FortPawn.h"
#include "Player/FortPlayerController.h"
#include "ReflectionParams.h"
#include "StructArray.h"
#include "UObject/Class.h"

void UFortMissionLibrary::ExecuteMissionEvent(UObject* WorldContextObject, FName EventType, UObject* Params)
{
    Reflection::FParams Parms(StaticFunction_ExecuteMissionEvent(), true);
    Parms.Set(L"WorldContextObject", WorldContextObject);
    Parms.Set(L"EventType", EventType);
    Parms.Set(L"Params", Params);
    Parms.Call(StaticClass()->GetDefaultObject());
}

void UFortMissionLibrary::ExecuteMissionEvent(UObject* WorldContextObject, FName EventType, const FGuid& MissionGuid, const FGameplayTagContainer& ObjectiveHandle, UObject* Params)
{
    Reflection::FParams Parms(StaticFunction_ExecuteMissionEvent(), true);
    Parms.Set(L"WorldContextObject", WorldContextObject);
    Parms.Set(L"EventType", EventType);
    Parms.Set(L"MissionGuid", MissionGuid);
    Parms.Set(L"ObjectiveHandle", ObjectiveHandle);
    Parms.Set(L"Params", Params);
    Parms.Call(StaticClass()->GetDefaultObject());
}

void UFortMissionLibrary::SpawnAndCollectPlayerPickups(UObject* WorldContextObject, AFortPlayerController* PlayerController, const FVector& StartPosition, FName LootTierGroup, int32_t LootTier)
{
#if WITH_SERVER_CODE
    UWorld* World = WorldContextObject ? WorldContextObject->GetWorld() : nullptr;
    AFortPawn* TargetPawn = PlayerController ? static_cast<AFortPawn*>(PlayerController->MyFortPawn) : nullptr;
    if (!World || !TargetPawn)
    {
        return;
    }

    Reflection::FParams Params(UFortKismetLibrary::StaticFunction_PickLootDrops(), true);
    Params.Set(L"TierGroupName", LootTierGroup);
    Params.Set(L"WorldLevel", -1);
    Params.Set(L"ForcedLootTier", LootTier);
    if (!Params.Call(UFortKismetLibrary::StaticClass()->GetDefaultObject()) || !Params.Take<bool>(L"ReturnValue"))
    {
        return;
    }

    TStructArray<FFortItemEntry>* LootToDrop = Params.Get<TStructArray<FFortItemEntry>>(L"OutLootToDrop");
    const FRotator SpawnRotation(0.0f, 0.0f, 0.0f);
    for (int32_t Index = 0; LootToDrop && Index < LootToDrop->Num(); ++Index)
    {
        if (AFortPickup* Pickup = AFortPickup::CreatePickup(World, (*LootToDrop)[Index], &StartPosition, &SpawnRotation))
        {
            Pickup->SetPickupTarget(TargetPawn);
        }
    }
#endif
}

DEFINE_FUNCTION(UFortMissionLibrary::execSpawnAndCollectPlayerPickups)
{
    P_GET_OBJECT(UObject, Z_Param_WorldContextObject);
    P_GET_OBJECT(AFortPlayerController, Z_Param_PlayerController);
    P_GET_STRUCT(FVector, Z_Param_StartPosition);
    P_GET_PROPERTY(UNameProperty, Z_Param_LootTierGroup);
    P_GET_PROPERTY(UIntProperty, Z_Param_LootTier);
    P_FINISH;
    P_NATIVE_BEGIN;
    SpawnAndCollectPlayerPickups(Z_Param_WorldContextObject, Z_Param_PlayerController, Z_Param_StartPosition, Z_Param_LootTierGroup, Z_Param_LootTier);
    P_NATIVE_END;
}

void UFortMissionLibrary::HookNatives()
{
    if (!Natives::FFrame_Step && !Finder::ResolveScriptFrame())
    {
        return;
    }

    if (UFunction* Function = StaticFunction_SpawnAndCollectPlayerPickups())
    {
        Function->SetNativeFunc(&execSpawnAndCollectPlayerPickups);
        Log::Info(L"Hook UFortMissionLibrary::SpawnAndCollectPlayerPickups: native");
    }
}
