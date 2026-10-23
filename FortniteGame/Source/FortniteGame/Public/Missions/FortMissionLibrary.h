#pragma once

#include "GameplayTagContainer.h"
#include "Missions/FortObjectiveBase.h"
#include "Misc/Guid.h"
#include "Math/Vector.h"
#include "UObject/NameTypes.h"
#include "UObject/Object.h"
#include "UObject/Stack.h"

class AFortPickup;
class AFortPlayerController;
class UFortAIEncounterInfo;
class UFortMissionInfo;
class UFortSchematicItemDefinition;
class UFortWorldItemDefinition;

class UFortMissionLibrary : public UObject
{
    DefineUnrealClass(UFortMissionLibrary, FortniteGame)
    DefineUFunction(ExecuteMissionEvent)
    DefineUFunction(LoadMission)
    DefineUFunction(SpawnAndCollectPlayerPickups)
    DefineUFunction(GiveMissionRewardsToPlayerAsPickups)
    DefineUFunction(SpawnMissionItemPickupInWorld)
    DefineUFunction(GiveMissionSchematicItemDirectlyToPlayer)
    DefineUFunction(StopAIEncounter)

public:
    static void ExecuteMissionEvent(UObject* WorldContextObject, FName EventType, UObject* Params);
    static void ExecuteMissionEvent(UObject* WorldContextObject, FName EventType, const FGuid& MissionGuid, const FGameplayTagContainer& ObjectiveHandle, UObject* Params);
    static void LoadMission(UObject* WorldContextObject, UFortMissionInfo* MissionInfo);
    static void SpawnAndCollectPlayerPickups(UObject* WorldContextObject, AFortPlayerController* PlayerController, const FVector& StartPosition, FName LootTierGroup, int32_t LootTier);

    static void GiveMissionRewardsToPlayerAsPickups(UObject* WorldContextObject, const FGuid& MissionGUID, AFortPlayerController* PlayerController, const FVector& StartPosition, float Radius, FName LootTierGroup, int32_t LootTier);
    static AFortPickup* SpawnMissionItemPickupInWorld(UObject* WorldContextObject, const FGuid& MissionGuid, UFortWorldItemDefinition* ItemDefinition, int32_t NumberToSpawn, const FVector& Position, const FVector& Direction, int32_t OverrideMaxStackCount);
    static bool GiveMissionSchematicItemDirectlyToPlayer(UObject* WorldContextObject, AFortPlayerController* TargetPlayer, const FGuid& MissionGuid, UFortSchematicItemDefinition* SchematicDefinition);
    static void StopAIEncounter(UObject* WorldContextObject, UFortAIEncounterInfo* ActiveEncounter, EFortObjectiveStatus EncounterStopReason);

    DECLARE_FUNCTION(execSpawnAndCollectPlayerPickups);
    DECLARE_FUNCTION(execGiveMissionRewardsToPlayerAsPickups);
    DECLARE_FUNCTION(execSpawnMissionItemPickupInWorld);
    DECLARE_FUNCTION(execGiveMissionSchematicItemDirectlyToPlayer);
    DECLARE_FUNCTION(execStopAIEncounter);

    static void HookNatives();
};
