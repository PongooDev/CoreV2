#include "pch.h"
#include "Missions/FortMissionLibrary.h"
#include "AI/FortAIDirector.h"
#include "AI/FortAIGoalManager.h"
#include "Engine/World.h"
#include "Finder.h"
#include "FortGameState.h"
#include "FortKismetLibrary.h"
#include "FortWorldManager.h"
#include "Items/FortItemDefinition.h"
#include "Items/FortLootPackage.h"
#include "Items/FortPickup.h"
#include "Items/FortSchematicItemDefinition.h"
#include "Log.h"
#include "Logging/LogMacros.h"
#include "Natives.h"
#include "Pawns/FortPawn.h"
#include "Player/FortPlayerController.h"
#include "Player/FortPlayerState.h"
#include "ReflectionParams.h"
#include "StructArray.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/ObjectMacros.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

#include <algorithm>
#include <vector>

DEFINE_LOG_CATEGORY_STATIC(LogFortAIDirector, Log, All);
DEFINE_LOG_CATEGORY_STATIC(LogFortMission, Log, All);

namespace
{
    UFunction* ClientGivePlayerLocalAccountItemFunction = nullptr;
    void (*ClientGivePlayerLocalAccountItem_Implementation)(AFortPlayerController* This, UFortItemDefinition* ItemDefinition, int32_t Count) = nullptr;

    // The zone's loot level, as UFortMissionLibrary::GiveMissionRewardsToPlayer (Cert 0x61E660) reads it for its tier
    // pick (0x61E6DC); -1, which lets every world level's rows drop, only without a world manager (#66).
    int32_t GetRewardWorldLevel(UWorld* World)
    {
        AFortGameState* GameState = World ? Cast<AFortGameState>(reinterpret_cast<UObject*>(World->GetGameState())) : nullptr;
        AFortWorldManager* WorldManager = GameState ? static_cast<AFortWorldManager*>(GameState->WorldManager) : nullptr;
        return WorldManager ? WorldManager->GetWorldLevel() : -1;
    }
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
    Params.Set(L"WorldLevel", GetRewardWorldLevel(World));
    Params.Set(L"ForcedLootTier", LootTier);
    if (!Params.Call(UFortKismetLibrary::StaticClass()->GetDefaultObject()) || !Params.Take<bool>(L"ReturnValue"))
    {
        return;
    }

    TStructArray<FFortItemEntry>* LootToDrop = Params.Get<TStructArray<FFortItemEntry>>(L"OutLootToDrop");
    if (LootToDrop)
    {
        AFortPickup::CombineItemEntries(*LootToDrop, PlayerController->WorldInventory);
    }
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

void UFortMissionLibrary::GiveMissionRewardsToPlayer(UObject* WorldContextObject, AFortPlayerController* PlayerController, FName LootTierGroup, int32_t LootTier)
{
#if WITH_SERVER_CODE
    // Cert's (0x61E660): the world's loot level, then for each player (the one given, else every player controller in
    // the world) with a FortPlayerState, the tier group's key and tier (0x5DF790, ForcedLootTier = LootTier) and that
    // tier's drops (0x5F1D70) with the player state's ability system and bonus drops allowed. Cert hands the drops'
    // world level the tier the pick chose, not the loot level (edx = the picked tier at 0x61EBE1), and Core does too.
    // The rest is cut even in Cert: the drops are rolled and released. Here they go to the player's world inventory as
    // K2_GiveItemToPlayer gives them (with a notification), at their rolled level; account items can't go there.
    UWorld* World = WorldContextObject ? WorldContextObject->GetWorld() : nullptr;
    if (!World || LootTierGroup.IsNone())
    {
        return;
    }

    const int32_t WorldLevel = GetRewardWorldLevel(World);
    std::vector<AFortPlayerController*> PlayerControllers;
    if (PlayerController)
    {
        PlayerControllers.push_back(PlayerController);
    }
    else
    {
        for (TObjectIterator<AFortPlayerController> It; It; ++It)
        {
            if (It->GetWorld() == World)
            {
                PlayerControllers.push_back(*It);
            }
        }
    }

    for (AFortPlayerController* Controller : PlayerControllers)
    {
        AFortPlayerState* PlayerState = Controller ? Cast<AFortPlayerState>(reinterpret_cast<UObject*>(static_cast<APlayerState*>(Controller->PlayerState))) : nullptr;
        if (!PlayerState)
        {
            continue;
        }

        FName LootTierKey;
        int32_t PickedLootTier = LootTier;
        const FGameplayTagContainer ContextTags;
        if (!UFortKismetLibrary::PickLootTierKeyAndLootTierFromTierGroup(LootTierKey, PickedLootTier, LootTierGroup, WorldLevel, LootTier, ContextTags))
        {
            UE_LOG(LogFortMission, Warning, TEXT("GiveMissionRewardsToPlayer: no loot tier in %s for world level %d"), *LootTierGroup.ToString(), WorldLevel);
            continue;
        }

        TStructArray<FFortItemEntry> LootToDrop{};
        UFortLootPackage::PickLootDrops(LootToDrop, PickedLootTier, LootTierKey, PlayerState->AbilitySystemComponent, ContextTags, true, false);
        int32_t Given = 0;
        for (int32_t Index = 0; Index < LootToDrop.Num(); ++Index)
        {
            FFortItemEntry& Entry = LootToDrop[Index];
            UFortItemDefinition* ItemDefinition = Entry.ItemDefinition;
            UFortWorldItemDefinition* WorldItemDefinition = Cast<UFortWorldItemDefinition>(ItemDefinition);
            if (WorldItemDefinition && Entry.Count > 0)
            {
                UFortKismetLibrary::K2_GiveItemToPlayer(Controller, WorldItemDefinition, Entry.Count, true, std::max(static_cast<int32_t>(Entry.Level), 1));
                ++Given;
            }
        }
        UE_LOG(LogFortMission, Log, TEXT("GiveMissionRewardsToPlayer: %s gets %d of %d drops from %s (%s)"), *Controller->GetName(), Given, LootToDrop.Num(), *LootTierGroup.ToString(), *LootTierKey.ToString());
        LootToDrop.Empty();
    }
#endif
}

DEFINE_FUNCTION(UFortMissionLibrary::execGiveMissionRewardsToPlayer)
{
    // Its parameters differ between builds (7.x added RewardDisplayTime; the event's guid and the two texts only name the
    // reward), so they are stepped by the build's own UFunction and read by name.
    UFunction* Function = StaticFunction_GiveMissionRewardsToPlayer();
    std::vector<uint8_t> Parms(std::max<size_t>(Function->ParmsSize, Function->GetPropertiesSize()) + sizeof(uint64_t), 0);
    Function->InitializeStruct(Parms.data());
    for (UField* Field = static_cast<UField*>(Function->Children); Field; Field = Field->Next)
    {
        UProperty* Property = static_cast<UProperty*>(Field);
        if ((Property->PropertyFlags & (CPF_Parm | CPF_ReturnParm)) == CPF_Parm)
        {
            P_STACK.StepCompiledIn<UProperty>(Parms.data() + Property->GetOffset_ForInternal());
        }
    }
    P_FINISH;
    P_NATIVE_BEGIN;
    const auto Parm = [Function, &Parms](const wchar_t* Name, size_t Size) -> const void*
    {
        UProperty* Property = Function->FindPropertyByName(FName(Name, FNAME_Find));
        return Property && static_cast<size_t>(Property->GetSize()) == Size ? Parms.data() + Property->GetOffset_ForInternal() : nullptr;
    };
    const void* WorldContextObject = Parm(L"WorldContextObject", sizeof(UObject*));
    const void* PlayerController = Parm(L"PlayerController", sizeof(AFortPlayerController*));
    const void* LootTierGroup = Parm(L"LootTierGroup", sizeof(FName));
    const void* LootTier = Parm(L"LootTier", sizeof(int32_t));
    GiveMissionRewardsToPlayer(WorldContextObject ? *static_cast<UObject* const*>(WorldContextObject) : nullptr, PlayerController ? *static_cast<AFortPlayerController* const*>(PlayerController) : nullptr,
        LootTierGroup ? *static_cast<const FName*>(LootTierGroup) : FName(), LootTier ? *static_cast<const int32_t*>(LootTier) : -1);
    P_NATIVE_END;
    Function->DestroyStruct(Parms.data());
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
    Params.Set(L"WorldLevel", GetRewardWorldLevel(World));
    Params.Set(L"ForcedLootTier", LootTier);
    if (!Params.Call(UFortKismetLibrary::StaticClass()->GetDefaultObject()) || !Params.Take<bool>(L"ReturnValue"))
    {
        return;
    }

    TStructArray<FFortItemEntry>* LootToDrop = Params.Get<TStructArray<FFortItemEntry>>(L"OutLootToDrop");
    if (LootToDrop)
    {
        AFortPickup::CombineItemEntries(*LootToDrop, PlayerController->WorldInventory);
    }
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
    // GiveMissionRewardsToPlayer's roll went with the rest of the loot code where the build's PickLootDrops is cut (1.2
    // on): 7.10's (0x10DC270, from its exec 0x162C210) only formats the reward's title and description texts and gives
    // nothing, so Frostnite's starting kit never came (#491). Cert's still rolls (the loot package pick and its FindRow
    // context, "UFortLootPackage::PickLootDrops", are in the build), and is left alone.
    if (UFunction* Function = FindStaticFunction_GiveMissionRewardsToPlayer(); Function && Finder::StringRefs(L"UFortLootPackage::PickLootDrops").empty())
    {
        Function->SetNativeFunc(&execGiveMissionRewardsToPlayer);
        Log::Info(L"Hook UFortMissionLibrary::GiveMissionRewardsToPlayer: native");
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
