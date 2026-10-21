#include "pch.h"
#include "Items/FortBadgeItemDefinition.h"
#include "ReflectionParams.h"
#include "UObject/Class.h"

int32_t UFortBadgeItemDefinition::GetMissionPoints()
{
    Reflection::FParams Params(StaticFunction_GetMissionPoints());
    return Params.Call(this) ? Params.Take<int32_t>(L"ReturnValue") : 0;
}
