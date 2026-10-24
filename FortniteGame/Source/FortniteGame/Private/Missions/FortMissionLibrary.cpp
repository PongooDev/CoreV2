#include "pch.h"
#include "Missions/FortMissionLibrary.h"
#include "AI/FortAIDirector.h"
#include "AI/FortAIGoalManager.h"
#include "Engine/World.h"
#include "Finder.h"
#include "FortKismetLibrary.h"
#include "Items/FortItemDefinition.h"
#include "Items/FortPickup.h"
#include "Items/FortSchematicItemDefinition.h"
#include "Log.h"
#include "Logging/LogMacros.h"
#include "Natives.h"
#include "Pawns/FortPawn.h"
#include "Player/FortPlayerController.h"
#include "ReflectionParams.h"
#include "StructArray.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"

#include <vector>

DEFINE_LOG_CATEGORY_STATIC(LogFortAIDirector, Log, All);

namespace
{
    UFunction* ClientGivePlayerLocalAccountItemFunction = nullptr;
    void (*ClientGivePlayerLocalAccountItem_Implementation)(AFortPlayerController* This, UFortItemDefinition* ItemDefinition, int32_t Count) = nullptr;
}

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

void UFortMissionLibrary::LoadMission(UObject* WorldContextObject, UFortMissionInfo* MissionInfo)
{
    Reflection::FParams Parms(StaticFunction_LoadMission(), true);
    Parms.Set(L"WorldContextObject", WorldContextObject);
    Parms.Set(L"MissionInfo", MissionInfo);
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

void UFortMissionLibrary::GiveMissionRewardsToPlayerAsPickups(UObject* WorldContextObject, const FGuid& MissionGUID, AFortPlayerController* PlayerController, const FVector& StartPosition, float Radius, FName LootTierGroup, int32_t LootTier)
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
        const float Angle = Natives::FMath_SRand() * 6.2831853f;
        const float Distance = Natives::FMath_SRand() * Radius;
        const FVector FinalLocation(StartPosition.X + std::cos(Angle) * Distance, StartPosition.Y + std::sin(Angle) * Distance, StartPosition.Z);
        if (AFortPickup* Pickup = AFortPickup::CreatePickup(World, (*LootToDrop)[Index], &StartPosition, &SpawnRotation, PlayerController))
        {
            Pickup->SetOptionalMissionGuid(MissionGUID);
            Pickup->TossPickup(FinalLocation, TargetPawn, 0);
        }
    }
#endif
}

DEFINE_FUNCTION(UFortMissionLibrary::execGiveMissionRewardsToPlayerAsPickups)
{
    P_GET_OBJECT(UObject, Z_Param_WorldContextObject);
    P_GET_STRUCT(FGuid, Z_Param_MissionGUID);
    P_GET_OBJECT(AFortPlayerController, Z_Param_PlayerController);
    P_GET_STRUCT(FVector, Z_Param_StartPosition);
    P_GET_PROPERTY(UFloatProperty, Z_Param_Radius);
    P_GET_PROPERTY(UNameProperty, Z_Param_LootTierGroup);
    P_GET_PROPERTY(UIntProperty, Z_Param_LootTier);
    P_FINISH;
    P_NATIVE_BEGIN;
    GiveMissionRewardsToPlayerAsPickups(Z_Param_WorldContextObject, Z_Param_MissionGUID, Z_Param_PlayerController, Z_Param_StartPosition, Z_Param_Radius, Z_Param_LootTierGroup, Z_Param_LootTier);
    P_NATIVE_END;
}

AFortPickup* UFortMissionLibrary::SpawnMissionItemPickupInWorld(UObject* WorldContextObject, const FGuid& MissionGuid, UFortWorldItemDefinition* ItemDefinition, int32_t NumberToSpawn, const FVector& Position, const FVector& Direction, int32_t OverrideMaxStackCount)
{
#if WITH_SERVER_CODE
    UWorld* World = WorldContextObject ? WorldContextObject->GetWorld() : nullptr;
    if (!World || !ItemDefinition || NumberToSpawn <= 0)
    {
        return nullptr;
    }

    UScriptStruct* EntryStruct = FFortItemEntry::StaticStruct();
    std::vector<uint8_t> EntryStorage(EntryStruct->GetStructureSize(), 0);
    FFortItemEntry* ItemEntry = reinterpret_cast<FFortItemEntry*>(EntryStorage.data());
    FFortItemEntry::Construct(ItemEntry, ItemDefinition, NumberToSpawn, 0);

    const FRotator SpawnRotation = Direction.Rotation();
    AFortPickup* Pickup = AFortPickup::CreatePickup(World, *ItemEntry, &Position, &SpawnRotation);
    EntryStruct->DestroyStruct(ItemEntry);

    if (Pickup)
    {
        Pickup->SetOptionalMissionGuid(MissionGuid);
        Pickup->TossPickup(Position, nullptr, OverrideMaxStackCount);
    }
    return Pickup;
#else
    return nullptr;
#endif
}

DEFINE_FUNCTION(UFortMissionLibrary::execSpawnMissionItemPickupInWorld)
{
    P_GET_OBJECT(UObject, Z_Param_WorldContextObject);
    P_GET_STRUCT(FGuid, Z_Param_MissionGuid);
    P_GET_OBJECT(UFortWorldItemDefinition, Z_Param_ItemDefinition);
    P_GET_PROPERTY(UIntProperty, Z_Param_NumberToSpawn);
    P_GET_STRUCT(FVector, Z_Param_Position);
    P_GET_STRUCT(FVector, Z_Param_Direction);
    P_GET_PROPERTY(UIntProperty, Z_Param_OverrideMaxStackCount);
    P_FINISH;
    P_NATIVE_BEGIN;
    *static_cast<AFortPickup**>(RESULT_PARAM) = SpawnMissionItemPickupInWorld(Z_Param_WorldContextObject, Z_Param_MissionGuid, Z_Param_ItemDefinition, Z_Param_NumberToSpawn, Z_Param_Position, Z_Param_Direction, Z_Param_OverrideMaxStackCount);
    P_NATIVE_END;
}

bool UFortMissionLibrary::GiveMissionSchematicItemDirectlyToPlayer(UObject* WorldContextObject, AFortPlayerController* TargetPlayer, const FGuid& MissionGuid, UFortSchematicItemDefinition* SchematicDefinition)
{
#if WITH_SERVER_CODE
    if (!TargetPlayer || !SchematicDefinition || TargetPlayer->Role != ROLE_Authority || !ClientGivePlayerLocalAccountItem_Implementation)
    {
        return false;
    }

    ClientGivePlayerLocalAccountItem_Implementation(TargetPlayer, SchematicDefinition, 1);

    Reflection::FParams Params(ClientGivePlayerLocalAccountItemFunction, true);
    Params.Set(L"ItemDefinition", static_cast<UFortItemDefinition*>(SchematicDefinition));
    Params.Set(L"Count", 1);
    Params.Call(TargetPlayer);
    return true;
#else
    return false;
#endif
}

DEFINE_FUNCTION(UFortMissionLibrary::execGiveMissionSchematicItemDirectlyToPlayer)
{
    P_GET_OBJECT(UObject, Z_Param_WorldContextObject);
    P_GET_OBJECT(AFortPlayerController, Z_Param_TargetPlayer);
    P_GET_STRUCT(FGuid, Z_Param_MissionGuid);
    P_GET_OBJECT(UFortSchematicItemDefinition, Z_Param_SchematicDefinition);
    P_FINISH;
    P_NATIVE_BEGIN;
    *static_cast<bool*>(RESULT_PARAM) = GiveMissionSchematicItemDirectlyToPlayer(Z_Param_WorldContextObject, Z_Param_TargetPlayer, Z_Param_MissionGuid, Z_Param_SchematicDefinition);
    P_NATIVE_END;
}

void UFortMissionLibrary::StopAIEncounter(UObject* WorldContextObject, UFortAIEncounterInfo* ActiveEncounter, EFortObjectiveStatus EncounterStopReason)
{
#if WITH_SERVER_CODE
    UWorld* World = WorldContextObject ? WorldContextObject->GetWorld() : nullptr;
    if (World && ActiveEncounter)
    {
        if (AFortAIDirector* AIDirector = AFortAIDirector::GetCurrent(World))
        {
            AIDirector->StopEncounter(ActiveEncounter, EncounterStopReason);
        }
    }
    else
    {
        UE_LOG(LogFortAIDirector, Warning, TEXT("UFortMissionLibrary::StopAIEncounter: No ActiveEncounter or WorldContext!"));
    }
#endif
}

DEFINE_FUNCTION(UFortMissionLibrary::execStopAIEncounter)
{
    P_GET_OBJECT(UObject, Z_Param_WorldContextObject);
    P_GET_OBJECT(UFortAIEncounterInfo, Z_Param_ActiveEncounter);
    P_GET_PROPERTY(UByteProperty, Z_Param_EncounterStopReason);
    P_FINISH;
    P_NATIVE_BEGIN;
    StopAIEncounter(Z_Param_WorldContextObject, Z_Param_ActiveEncounter, EFortObjectiveStatus{ Z_Param_EncounterStopReason });
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
    if (UFunction* Function = StaticFunction_GiveMissionRewardsToPlayerAsPickups())
    {
        Function->SetNativeFunc(&execGiveMissionRewardsToPlayerAsPickups);
        Log::Info(L"Hook UFortMissionLibrary::GiveMissionRewardsToPlayerAsPickups: native");
    }
    if (UFunction* Function = StaticFunction_SpawnMissionItemPickupInWorld())
    {
        Function->SetNativeFunc(&execSpawnMissionItemPickupInWorld);
        Log::Info(L"Hook UFortMissionLibrary::SpawnMissionItemPickupInWorld: native");
    }
    if (UFunction* Function = StaticFunction_StopAIEncounter())
    {
        Function->SetNativeFunc(&execStopAIEncounter);
        Log::Info(L"Hook UFortMissionLibrary::StopAIEncounter: native");
    }

    ClientGivePlayerLocalAccountItemFunction = FindObject<UFunction>(nullptr, L"/Script/FortniteGame.FortPlayerController:ClientGivePlayerLocalAccountItem");
    const int32_t LocalAccountItemIndex = Finder::ImplementationIndex(ClientGivePlayerLocalAccountItemFunction);
    UObject* PlayerControllerDefault = LocalAccountItemIndex >= 0 ? AFortPlayerController::StaticClass()->GetDefaultObject(false) : nullptr;
    if (!PlayerControllerDefault)
    {
        Log::Error(L"Finder: AFortPlayerController::ClientGivePlayerLocalAccountItem_Implementation slot not found");
        return;
    }
    ClientGivePlayerLocalAccountItem_Implementation = reinterpret_cast<decltype(ClientGivePlayerLocalAccountItem_Implementation)>((*reinterpret_cast<void***>(PlayerControllerDefault))[LocalAccountItemIndex]);

    if (UFunction* Function = StaticFunction_GiveMissionSchematicItemDirectlyToPlayer())
    {
        Function->SetNativeFunc(&execGiveMissionSchematicItemDirectlyToPlayer);
        Log::Info(L"Hook UFortMissionLibrary::GiveMissionSchematicItemDirectlyToPlayer: native");
    }
}
