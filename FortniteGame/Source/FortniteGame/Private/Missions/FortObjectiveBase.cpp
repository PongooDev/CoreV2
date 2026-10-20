#include "pch.h"
#include "Missions/FortObjectiveBase.h"
#include "Finder.h"
#include "Hooks.h"
#include "Logging/LogMacros.h"
#include "Missions/FortMission.h"
#include "Natives.h"
#include "ReflectionParams.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"

DEFINE_LOG_CATEGORY_STATIC(LogFortMission, Log, All);

namespace
{
    const wchar_t* ObjectiveStatusName(EFortObjectiveStatus Status)
    {
        switch (Status)
        {
        case EFortObjectiveStatus::Created:
            return TEXT("Created");
        case EFortObjectiveStatus::InProgress:
            return TEXT("InProgress");
        case EFortObjectiveStatus::Succeeded:
            return TEXT("Succeeded");
        case EFortObjectiveStatus::Failed:
            return TEXT("Failed");
        case EFortObjectiveStatus::NeutralCompletion:
            return TEXT("NeutralCompletion");
        default:
            return TEXT("Max_None");
        }
    }
}

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
    UE_LOG(LogFortMission, Log, TEXT("Objective %s: New Objective status is %s"), *GetName(), ObjectiveStatusName(NewStatus));

    if (GetNetMode() != NM_DedicatedServer)
    {
        Reflection::FParams Params(StaticFunction_OnRep_ObjectiveStatus());
        Params.Call(this);
    }

    if (AFortMission* Mission = GetMission())
    {
        Mission->ObjectiveStatusUpdated(this, NewStatus);
    }
#endif
}

bool AFortObjectiveBase::StartPlayingObjective()
{
#if WITH_SERVER_CODE
    if (Role != ROLE_Authority || ObjectiveStatus != EFortObjectiveStatus::Created)
    {
        return false;
    }

    SetObjectiveStatus(EFortObjectiveStatus::InProgress);
    return true;
#else
    return false;
#endif
}

void AFortObjectiveBase::StopObjective(EFortObjectiveStatus Status, const FText& Description, UObject* ExtraBadgeToGrant)
{
#if WITH_SERVER_CODE
    if (Role != ROLE_Authority || ObjectiveStatus != EFortObjectiveStatus::InProgress)
    {
        return;
    }

    ClearObjectiveTimer();
    SetObjectiveStatus(Status);
#endif
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
}

bool AFortObjectiveBase::StartPlayingObjectiveHK(AFortObjectiveBase* This)
{
    return This->StartPlayingObjective();
}

void AFortObjectiveBase::StopObjectiveHK(AFortObjectiveBase* This, EFortObjectiveStatus Status, const FText& Description, UObject* ExtraBadgeToGrant)
{
    This->StopObjective(Status, Description, ExtraBadgeToGrant);
}
