#include "pch.h"
#include "Missions/FortMission.h"
#include "AI/FortAIDirector.h"
#include "Finder.h"
#include "Hooks.h"
#include "Log.h"
#include "Engine/World.h"
#include "Items/FortBadgeItemDefinition.h"
#include "Missions/FortMissionInfo.h"
#include "Missions/FortMissionManager.h"
#include "Templates/Casts.h"
#include "UObject/UObjectIterator.h"
#include "ReflectionParams.h"
#include "Natives.h"
#include "ObjectLayout.h"
#include "UObject/Class.h"

void AFortMission::HookCallSites()
{
    for (UFunction* Function : { StaticFunction_StopMissionAIEncounter(), StaticFunction_StopEncounterSequence() })
    {
        if (uint8_t* Call = Finder::NullGetCurrentCall(Function))
        {
            Hooks::RedirectCall(L"AFortAIDirector::GetCurrent", Call, reinterpret_cast<void*>(&AFortAIDirector::GetCurrent));
        }
        else
        {
            Log::Error(L"Finder: AFortAIDirector::GetCurrent call in {} not found", Function ? Function->GetName().ToStdString() : std::wstring(L"(none)"));
        }
    }

    if (uint8_t* Call = Finder::SharedEmptyCallInImplementation(AFortObjectiveBase::StaticFunction_GrantRewardsByTag()))
    {
        Hooks::RedirectCall(L"AFortObjectiveBase::GrantRewardsByTag -> AFortMission::GrantRewardsByTag", Call, reinterpret_cast<void*>(&GrantRewardsByTagCall));
    }
    else
    {
        Log::Error(L"Finder: AFortMission::GrantRewardsByTag call in AFortObjectiveBase::GrantRewardsByTag not found");
    }
}

ptrdiff_t AFortMission::MissionInfoOffset()
{
    return Member_MissionInfo.GetOffsetChecked();
}

bool AFortMission::IsPrimaryMission() const
{
    UFortMissionInfo* Info = MissionInfo;
    return Info && Info->MissionType == 0;
}

TArray<AFortPlayerController*> AFortMission::GetParticipatingMissionControllers()
{
    Reflection::FParams Params(StaticFunction_GetParticipatingMissionControllers(), true);
    return Params.Call(this) ? Params.Take<TArray<AFortPlayerController*>>(L"ReturnValue") : TArray<AFortPlayerController*>();
}

ptrdiff_t AFortMission::ActiveObjectivesOffset()
{
    return Member_ActiveObjectives.GetOffsetChecked();
}

void AFortMission::ObjectiveStatusUpdated(AFortObjectiveBase* Objective, EFortObjectiveStatus NewStatus)
{
    if (Natives::AFortMission_ObjectiveStatusUpdatedIndex < 0 && !Finder::ResolveMissionObjectivesLive())
    {
        return;
    }
    CallVirtual<void>(this, Natives::AFortMission_ObjectiveStatusUpdatedIndex, Objective, NewStatus);
}

AFortMissionManager* AFortMission::GetMissionManager() const
{
    UWorld* World = GetWorld();
    for (TObjectIterator<AFortMissionManager> It; It; ++It)
    {
        if (It->GetWorld() == World && !It->IsPendingKill())
        {
            return *It;
        }
    }
    return nullptr;
}

TArray<UFortWorldItemDefinition*> AFortMission::GetRewardItemsByTag(const FGameplayTag& RewardTag)
{
    Reflection::FParams Params(StaticFunction_GetRewardItemsByTag(), true);
    Params.Set(L"RewardTag", RewardTag);
    TArray<UFortWorldItemDefinition*> Items;
    if (Params.Call(this))
    {
        Items = Params.Take<TArray<UFortWorldItemDefinition*>>(L"OutRewardItems");
    }
    return Items;
}

void AFortMission::GrantRewardsByTag(FGameplayTag RewardTag, EFortRewardType RewardType)
{
#if WITH_SERVER_CODE
    AFortMissionManager* MissionManager = Role == ROLE_Authority ? GetMissionManager() : nullptr;
    if (!MissionManager)
    {
        return;
    }

    const TArray<AFortPlayerController*> Players = GetParticipatingMissionControllers();
    for (UFortWorldItemDefinition* Item : GetRewardItemsByTag(RewardTag))
    {
        if (UFortBadgeItemDefinition* Badge = Cast<UFortBadgeItemDefinition>(reinterpret_cast<UObject*>(Item)))
        {
            MissionManager->GrantBadge(Badge, RewardType, Players);
        }
    }
#endif
}

void AFortMission::GrantRewardsByTagCall(AFortMission* This, FGameplayTag RewardTag, EFortRewardType RewardType)
{
    This->GrantRewardsByTag(RewardTag, RewardType);
}

DEFINE_FUNCTION(AFortMission::execGrantRewardsByTag)
{
    P_GET_STRUCT(FGameplayTag, Z_Param_RewardTag);
    P_GET_PROPERTY(UByteProperty, Z_Param_RewardType);
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<AFortMission*>(Context)->GrantRewardsByTag(Z_Param_RewardTag, static_cast<EFortRewardType>(Z_Param_RewardType));
    P_NATIVE_END;
}

void AFortMission::HookNatives()
{
    if (!Natives::FFrame_Step && !Finder::ResolveScriptFrame())
    {
        return;
    }

    if (UFunction* Function = StaticFunction_GrantRewardsByTag())
    {
        Function->SetNativeFunc(&execGrantRewardsByTag);
        Log::Info(L"Hook AFortMission::GrantRewardsByTag: native");
    }
}
