#pragma once

#include "Items/FortItem.h"
#include "Misc/Guid.h"
#include "StructArray.h"

class EFortRewardActivityType
{
    DefineUnrealEnum(EFortRewardActivityType, FortniteGame)
    DefineUEnumValue(General)
    DefineUEnumValue(MissionPrimary)
    DefineUEnumValue(MissionSecondary)
    DefineUEnumValue(AccountLevelUp)
    DefineUEnumValue(Max_None)
};

class EFortCompletionResult
{
    DefineUnrealEnum(EFortCompletionResult, FortniteGame)
    DefineUEnumValue(Win)
    DefineUEnumValue(Loss)
    DefineUEnumValue(Draw)
    DefineUEnumValue(Undefined)
};

struct FFortRewardActivity
{
    DefineUnrealStruct(FFortRewardActivity, FortniteGame)
    DefineUProperty(EFortRewardActivityType, ActivityType)
    DefineUProperty(FGuid, ActivityGuid)
    DefineUProperty(float, RewardDisplayTime)
    DefineUStructArrayProperty(FFortItemEntry, RewardItems)
    DefineUStructArrayProperty(FFortItemEntry, MissedRewardItems)
    DefineUProperty(EFortCompletionResult, ActivityCompletionResult)
    DefineUProperty(int32_t, AdditionalCompletionMissionPoints)
};

struct FFortRewardReport
{
    DefineUnrealStruct(FFortRewardReport, FortniteGame)
    DefineUStructArrayProperty(FFortRewardActivity, RewardActivities)
    DefineUProperty(bool, bIsFinalized)
};
