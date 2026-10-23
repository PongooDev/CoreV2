#pragma once

#include "Containers/ScriptArray.h"
#include "Engine/NetSerialization.h"
#include "GameFramework/Actor.h"
#include "GameFramework/OnlineReplStructs.h"
#include "Misc/Guid.h"
#include "Missions/FortMissionInfo.h"
#include "StructArray.h"

class UFortMissionGenerator;
struct FFortQuestEarnedBadgeData;
class EFortMissionStatus;

class EMissionGenerationCategory
{
    DefineUnrealEnum(EMissionGenerationCategory, FortniteGame)
    DefineUEnumValue(Primary)
    DefineUEnumValue(Secondary)
    DefineUEnumValue(Tertiary)
    DefineUEnumValue(Survivor)
    DefineUEnumValue(Max_None)
};

struct FFortMissionEntry
{
    DefineUnrealStruct(FFortMissionEntry, FortniteGame)
    DefineUProperty(float, Weight)
    DefineUProperty(int32_t, WorldMinLevel)
    DefineUProperty(int32_t, WorldMaxLevel)
    DefineUProperty(UFortMissionGenerator*, MissionGenerator)
    DefineUProperty(UFortMissionInfo*, MissionInfo)
    DefineUProperty(EMissionGenerationCategory, GenerationCategory)
    DefineUStructProperty(FFortGeneratedDifficultyOptions, GeneratedDifficultyOptions)
    DefineUProperty(FScriptArray, BlueprintLookupData)
};

struct FFortMissionRecord
{
    DefineUnrealStruct(FFortMissionRecord, FortniteGame)
    DefineUStructProperty(FFortMissionEntry, MissionEntry)
    DefineUProperty(UFortMissionGenerator*, MissionGenerator)
    DefineUProperty(FGuid, MissionGuid)
};

struct FFortMissionManagerRecord
{
    DefineUnrealStruct(FFortMissionManagerRecord, FortniteGame)
    DefineUProperty(UClass*, MissionManagerClass)
    DefineUStructArrayProperty(FFortMissionRecord, MissionRecords)
    DefineUProperty(int32_t, NumRequiredMissionsOfType)

    static void Construct(FFortMissionManagerRecord* This, UClass* InMissionManagerClass);
};

class AFortMission;
class AFortPlayerController;
class UFortBadgeItemDefinition;

class EFortRewardType
{
    DefineUnrealEnum(EFortRewardType, FortniteGame)
    DefineUEnumValue(Default)
    DefineUEnumValue(Missed)
    DefineUEnumValue(Max_None)
};

struct FEarnedBadgePlayerData
{
    DefineUnrealStruct(FEarnedBadgePlayerData, FortniteGame)
    DefineUProperty(FUniqueNetIdRepl, PlayerID)
    DefineUProperty(int32_t, Count)
};

struct FEarnedBadgeEntry : public FFastArraySerializerItem
{
    DefineUnrealStruct(FEarnedBadgeEntry, FortniteGame)
    DefineUProperty(UFortBadgeItemDefinition*, Badge)
    DefineUStructArrayProperty(FEarnedBadgePlayerData, PlayerData)
    DefineUProperty(EFortRewardType, RewardType)
};

struct FEarnedBadgeEntryArray : public FFastArraySerializer
{
    DefineUnrealStruct(FEarnedBadgeEntryArray, FortniteGame)
    DefineUStructArrayProperty(FEarnedBadgeEntry, Items)
};

class AFortMissionManager : public AActor
{
    DefineUnrealClass(AFortMissionManager, FortniteGame)

public:
    DefineUProperty(TArray<AFortMission*>, Missions)
    DefineUStructProperty(FEarnedBadgeEntryArray, EarnedBadgesArray)

    void OnZoneEnd(bool bZoneCompleted);
    void OnMissionStatusChange(AFortMission* Mission, EFortMissionStatus NewStatus);
    void GrantBadge(UFortBadgeItemDefinition* Badge, EFortRewardType RewardType, const TArray<AFortPlayerController*>& Players);
    void SetMissionCompletionResult(AFortMission* Mission);
    void SyncMissionRewardItems(AFortPlayerController* PlayerController);
    int32_t GetMissionPointsForPlayer(AFortPlayerController* PlayerController, int32_t MaxMissionPoints);
    void CollectEarnedBadgeData(AFortPlayerController* PlayerController, TStructArray<FFortQuestEarnedBadgeData>& OutBadges);
    static ptrdiff_t EarnedBadgesArrayOffset();

    static void HookVTables();

private:
    static void OnZoneEndHK(AFortMissionManager* This, bool bZoneCompleted);
    static inline void (*OnMissionStatusChangeOG)(AFortMissionManager* This, AFortMission* Mission, EFortMissionStatus NewStatus) = nullptr;
    static void OnMissionStatusChangeHK(AFortMissionManager* This, AFortMission* Mission, EFortMissionStatus NewStatus);
};
