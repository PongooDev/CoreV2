#include "pch.h"
#include "Missions/FortObjectiveBase.h"
#include "Finder.h"
#include "FortKismetLibrary.h"
#include "Items/FortItemDefinition.h"
#include "Hooks.h"
#include "Logging/LogMacros.h"
#include "Missions/FortMission.h"
#include "Items/FortBadgeItemDefinition.h"
#include "Missions/FortMissionLibrary.h"
#include "Missions/FortMissionManager.h"
#include "Missions/FortObjectiveFailedParams.h"
#include "Missions/FortObjectiveNeutralCompleteParams.h"
#include "Missions/FortObjectiveSucceededParams.h"
#include "Natives.h"
#include "ReflectionParams.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"

DEFINE_LOG_CATEGORY_STATIC(LogFortMission, Log, All);

AFortMission* AFortObjectiveBase::GetMission() const
{
    return Cast<AFortMission>(GetOwner());
}

void AFortObjectiveBase::SetObjectiveStatus(EFortObjectiveStatus NewStatus)
{
#if WITH_SERVER_CODE
    if (Role != ROLE_Authority || ObjectiveStatus == NewStatus)
    {
        return;
    }

    ForceNetUpdate();
    ObjectiveStatus = NewStatus;
    UE_LOG(LogFortMission, Log, TEXT("Objective %s: New Objective status is %s"), *GetName(), NewStatus.ToString().c_str());

    Reflection::FParams Params(StaticFunction_OnRep_ObjectiveStatus());
    Params.Call(this);
#endif
}

bool AFortObjectiveBase::ActivateObjective()
{
#if WITH_SERVER_CODE
    if (bStartPlayingOnMissionStart)
    {
        return StartPlayingObjective();
    }
#endif
    return true;
}

bool AFortObjectiveBase::StartPlayingObjective()
{
#if WITH_SERVER_CODE
    if (Role != ROLE_Authority || ObjectiveStatus == EFortObjectiveStatus::GetInProgress())
    {
        return false;
    }

    SetObjectiveStatus(EFortObjectiveStatus::GetInProgress());
    BlueprintOnStartPlaying();

    TArray<UFortWorldItemDefinition*>& ItemsToGive = ItemsToGiveOnObjectiveStart;
    for (UFortWorldItemDefinition* ItemDefinition : ItemsToGive)
    {
        UFortKismetLibrary::K2_GiveItemToAllPlayers(this, ItemDefinition, 1, true);
    }

    if (AFortMission* Mission = GetMission())
    {
        Mission->ObjectiveStatusUpdated(this, EFortObjectiveStatus::GetInProgress());
    }
    return true;
#else
    return false;
#endif
}

void AFortObjectiveBase::StopObjective(EFortObjectiveStatus Status, const FText& Description, UObject* ExtraBadgeToGrant)
{
#if WITH_SERVER_CODE
    if (Role != ROLE_Authority || ObjectiveStatus != EFortObjectiveStatus::GetInProgress())
    {
        return;
    }

    ClearObjectiveTimer();
    SetObjectiveStatus(Status);
    BlueprintOnObjectiveEnd(Status);

    AFortMission* Mission = GetMission();
    UFortBadgeItemDefinition* RewardBadge = Status == EFortObjectiveStatus::GetSucceeded() ? static_cast<UFortBadgeItemDefinition*>(ObjectiveRewardBadge) : nullptr;
    UFortBadgeItemDefinition* ExtraBadge = Cast<UFortBadgeItemDefinition>(ExtraBadgeToGrant);
    if (Mission && RewardBadge)
    {
        Mission->GrantBadge(RewardBadge, EFortRewardType::GetDefault());
    }
    if (Mission && ExtraBadge)
    {
        Mission->GrantBadge(ExtraBadge, EFortRewardType::GetDefault());
    }
    SendObjectiveEndedEvent(Status);
    if (Mission)
    {
        Mission->ObjectiveStatusUpdated(this, Status);
    }
#endif
}

void AFortObjectiveBase::SendObjectiveEndedEvent(EFortObjectiveStatus Status)
{
#if WITH_SERVER_CODE
    AFortMission* Mission = GetMission();
    if (!Mission || (Status != EFortObjectiveStatus::GetSucceeded() && Status != EFortObjectiveStatus::GetFailed() && Status != EFortObjectiveStatus::GetNeutralCompletion()))
    {
        return;
    }

    UObject* TransientPackage = StaticFindObject(nullptr, nullptr, L"/Engine/Transient");
    UObject* Params = nullptr;
    const wchar_t* EventName = nullptr;
    if (Status == EFortObjectiveStatus::GetSucceeded())
    {
        UFortObjectiveSucceededParams* SucceededParams = NewObject<UFortObjectiveSucceededParams>(TransientPackage, UFortObjectiveSucceededParams::StaticClass());
        Params = SucceededParams ? SucceededParams->SetParams(this) : nullptr;
        EventName = L"ObjectiveSucceeded";
    }
    else if (Status == EFortObjectiveStatus::GetFailed())
    {
        UFortObjectiveFailedParams* FailedParams = NewObject<UFortObjectiveFailedParams>(TransientPackage, UFortObjectiveFailedParams::StaticClass());
        Params = FailedParams ? FailedParams->SetParams(this) : nullptr;
        EventName = L"ObjectiveFailed";
    }
    else
    {
        UFortObjectiveNeutralCompleteParams* NeutralParams = NewObject<UFortObjectiveNeutralCompleteParams>(TransientPackage, UFortObjectiveNeutralCompleteParams::StaticClass());
        Params = NeutralParams ? NeutralParams->SetParams(this) : nullptr;
        EventName = L"ObjectiveNeutralCompletion";
    }

    const FName EventType(EventName, FNAME_Find);
    UFortMissionLibrary::ExecuteMissionEvent(this, EventType, Mission->MissionGuid, ObjectiveHandle, Params);
#endif
}

void AFortObjectiveBase::BlueprintOnStartPlaying()
{
    Reflection::FParams Params(FindFunction(StaticFunction_BlueprintOnStartPlaying()->GetFName()));
    Params.Call(this);
}

void AFortObjectiveBase::BlueprintOnObjectiveEnd(EFortObjectiveStatus Status)
{
    Reflection::FParams Params(FindFunction(StaticFunction_BlueprintOnObjectiveEnd()->GetFName()));
    Params.Set(L"Status", Status);
    Params.Call(this);
}

void AFortObjectiveBase::ClearObjectiveTimer()
{
    Reflection::FParams Params(StaticFunction_ClearObjectiveTimer());
    Params.Call(this);
}

void AFortObjectiveBase::HookVTables()
{
    UObject* ObjectiveDefault = StaticClass()->GetDefaultObject(false);
    void** ObjectiveVTable = ObjectiveDefault ? *reinterpret_cast<void***>(ObjectiveDefault) : nullptr;
    for (const auto& [Function, Implementation, Name] : {
             std::tuple{ StaticFunction_StartPlayingObjective(), reinterpret_cast<void*>(&StartPlayingObjectiveHK), L"AFortObjectiveBase::StartPlayingObjective" },
             std::tuple{ StaticFunction_StopObjective(), reinterpret_cast<void*>(&StopObjectiveHK), L"AFortObjectiveBase::StopObjective" },
         })
    {
        const int32_t Index = Finder::ImplementationIndex(Function);
        void* const Stripped = ObjectiveVTable && Index >= 0 ? ObjectiveVTable[Index] : nullptr;
        if (!Stripped)
        {
            Log::Error(L"Finder: {} slot not found", Name);
            continue;
        }

        for (TObjectIterator<UClass> It; It; ++It)
        {
            UClass* Class = *It;
            UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
            void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
            if (VTable && VTable[Index] == Stripped)
            {
                Hooks::SwapVTable(Name, VTable, Index, Implementation);
            }
        }
    }

    const int32_t ActivateIndex = Finder::ResolveObjectiveActivationLive(AFortMission::ActiveObjectivesOffset()) ? Natives::AFortObjectiveBase_ActivateObjectiveIndex : -1;
    void* const StrippedActivate = ObjectiveVTable && ActivateIndex >= 0 ? ObjectiveVTable[ActivateIndex] : nullptr;
    for (TObjectIterator<UClass> It; StrippedActivate && It; ++It)
    {
        UClass* Class = *It;
        UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
        void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
        if (VTable && VTable[ActivateIndex] == StrippedActivate)
        {
            Hooks::SwapVTable(L"AFortObjectiveBase::ActivateObjective", VTable, ActivateIndex, reinterpret_cast<void*>(&ActivateObjectiveHK));
        }
    }
}

bool AFortObjectiveBase::ActivateObjectiveHK(AFortObjectiveBase* This)
{
    return This->ActivateObjective();
}

bool AFortObjectiveBase::StartPlayingObjectiveHK(AFortObjectiveBase* This)
{
    return This->StartPlayingObjective();
}

void AFortObjectiveBase::StopObjectiveHK(AFortObjectiveBase* This, EFortObjectiveStatus Status, const FText& Description, UObject* ExtraBadgeToGrant)
{
    This->StopObjective(Status, Description, ExtraBadgeToGrant);
}
