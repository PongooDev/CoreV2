#pragma once

#include "Containers/EnumAsByte.h"
#include "Items/FortItem.h"
#include "Misc/Guid.h"
#include "StructArray.h"

namespace EFortRewardActivityType
{
    enum Type
    {
        General,
        MissionPrimary,
        MissionSecondary,
        AccountLevelUp,
        Max_None,
    };
}

enum class EFortCompletionResult : uint8_t
{
    Win,
    Loss,
    Draw,
    Undefined,
};

struct FFortRewardActivity
{
    DefineUnrealStruct(FFortRewardActivity, FortniteGame)
    DefineUProperty(TEnumAsByte<EFortRewardActivityType::Type>, ActivityType)
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
