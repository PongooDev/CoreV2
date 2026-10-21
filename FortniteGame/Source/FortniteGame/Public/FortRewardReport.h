#pragma once

#include "Items/FortItem.h"
#include "Misc/Guid.h"
#include "StructArray.h"

enum class EFortRewardActivityType : uint8_t
{
    General,
    MissionPrimary,
    MissionSecondary,
    AccountLevelUp,
    Max_None,
};

enum class EFortCompletionResult : uint8_t
{
    Win,
    Loss,
    Draw,
    Undefined,
    Max_None,
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
    DefineBitfieldUProperty(bIsFinalized)
};
