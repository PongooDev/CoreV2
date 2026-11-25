#include "pch.h"
#include "FortMissionTimerComponent.h"
#include "Finder.h"
#include "Engine/EngineTypes.h"
#include "GameFramework/Actor.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Log.h"
#include "Logging/LogMacros.h"
#include "ReflectionParams.h"
#include "UObject/Class.h"

#include <algorithm>
#include <cstring>

DEFINE_LOG_CATEGORY_STATIC(LogFortMissionTimer, Log, All);

void UFortMissionTimerComponent::PauseMissionTimer()
{
    Reflection::FParams Params(StaticFunction_PauseMissionTimer());
    Params.Call(this);
}

void UFortMissionTimerComponent::UnpauseMissionTimer()
{
    Reflection::FParams Params(StaticFunction_UnpauseMissionTimer());
    Params.Call(this);
}

bool UFortMissionTimerComponent::IsMissionTimerSet() const
{
    Reflection::FParams Params(StaticFunction_IsMissionTimerSet());
    return Params.Call(const_cast<UFortMissionTimerComponent*>(this)) && Params.Take<bool>(L"ReturnValue");
}

bool UFortMissionTimerComponent::IsMissionTimerPaused() const
{
    Reflection::FParams Params(StaticFunction_IsMissionTimerPaused());
    return Params.Call(const_cast<UFortMissionTimerComponent*>(this)) && Params.Take<bool>(L"ReturnValue");
}

float UFortMissionTimerComponent::GetMissionTimerTimeRemaining() const
{
    Reflection::FParams Params(StaticFunction_GetMissionTimerTimeRemaining());
    return Params.Call(const_cast<UFortMissionTimerComponent*>(this)) ? Params.Take<float>(L"ReturnValue") : 0.0f;
}

int32_t FMissionTimerData::TimerDelegateOffset()
{
    UScriptStruct* Struct = StaticStruct();
    if (!Struct || !Member_ClientRemainingTime.IsAvailable())
    {
        return -1;
    }
    const int32_t Offset = (Member_ClientRemainingTime.GetOffsetChecked() + static_cast<int32_t>(sizeof(float)) + 7) & ~7;
    // FTimerDynamicDelegate (0x10), then the FTimerHandle, and nothing else.
    return Struct->GetStructureSize() == Offset + 0x18 ? Offset : -1;
}

int32_t FMissionTimerData::TimerHandleOffset()
{
    const int32_t Delegate = TimerDelegateOffset();
    return Delegate >= 0 ? Delegate + 0x10 : -1;
}

void UFortMissionTimerComponent::AddOrRemoveTime(float TimeToAdd)
{
#if WITH_SERVER_CODE
    // As PauseMissionTimer (9.00 0x142F310): only the owner's authority, and only while a timer is set.
    AActor* Owner = GetOwner();
    if (!Owner || Owner->Role != ROLE_Authority || !IsMissionTimerSet())
    {
        return;
    }
    FMissionTimerData& Data = TimerData;
    uint8_t* DataBytes = reinterpret_cast<uint8_t*>(&Data);
    FTimerHandle& Handle = *reinterpret_cast<FTimerHandle*>(DataBytes + FMissionTimerData::TimerHandleOffset());

    // The timer runs out TimeToAdd later (earlier when negative). Core's: an end at or before now is put a moment
    // ahead, since a timer set to 0 is cleared without running and the objective would never end.
    const float Remaining = GetMissionTimerTimeRemaining();
    const float NewRemaining = std::max(Remaining + TimeToAdd, 0.01f);

    // K2_SetTimerDelegate finds the delegate's timer (K2_FindDynamicTimerHandle) and sets it again in place.
    Reflection::FParams SetTimer(UKismetSystemLibrary::FindStaticFunction_K2_SetTimerDelegate(), true);
    void* DelegateSlot = SetTimer.GetAddress(L"Delegate");
    UObject* SystemLibrary = UKismetSystemLibrary::StaticClass()->GetDefaultObject();
    if (!DelegateSlot || !SystemLibrary)
    {
        UE_LOG(LogFortMissionTimer, Warning, TEXT("AddOrRemoveTime: %s: K2_SetTimerDelegate not found"), *GetName());
        return;
    }
    std::memcpy(DelegateSlot, DataBytes + FMissionTimerData::TimerDelegateOffset(), 0x10);
    if (!SetTimer.Set(L"Time", NewRemaining) || !SetTimer.Set(L"bLooping", false) || !SetTimer.Call(SystemLibrary))
    {
        UE_LOG(LogFortMissionTimer, Warning, TEXT("AddOrRemoveTime: %s: the timer couldn't be set again"), *GetName());
        return;
    }
    Handle = SetTimer.Take<FTimerHandle>(L"ReturnValue");
    if (Data.bTimerIsPaused)
    {
        Reflection::FParams Pause(UKismetSystemLibrary::FindStaticFunction_K2_PauseTimerHandle(), true);
        if (!Pause.Set(L"WorldContextObject", static_cast<UObject*>(Owner)) || !Pause.Set(L"Handle", Handle) || !Pause.Call(SystemLibrary))
        {
            UE_LOG(LogFortMissionTimer, Warning, TEXT("AddOrRemoveTime: %s: the paused timer couldn't be paused again"), *GetName());
        }
    }

    // The timer's update (9.00 0x1432680): the replicated remaining time from the timer manager.
    Owner->FlushNetDormancy();
    Data.ReplicatedRemainingTime = GetMissionTimerTimeRemaining();
    // GetMissionTimerTimeElapsed adds TimeAddedOrRemoved, so the elapsed time runs on unbroken. A client's
    // OnRep_TimerData (9.00 0x142E400) broadcasts OnMissionTimerTimeAddedOrRemoved with LastTimeAddedOrRemoved, which
    // the timer's next update sets back to 0.
    Data.TimeAddedOrRemoved = Data.TimeAddedOrRemoved + TimeToAdd;
    Data.LastTimeAddedOrRemoved = TimeToAdd;
    struct
    {
        UFortMissionTimerComponent* TimerComponent;
        float TimeAdded;
    } Parameters{ this, TimeToAdd };
    OnMissionTimerTimeAddedOrRemoved.ProcessMulticastDelegate(&Parameters);
    UE_LOG(LogFortMissionTimer, Log, TEXT("AddOrRemoveTime: %s %+.1f s, %.1f s left (was %.1f s)"), *GetName(), TimeToAdd, Data.ReplicatedRemainingTime, Remaining);
#endif
}

DEFINE_FUNCTION(UFortMissionTimerComponent::execAddOrRemoveTime)
{
    P_GET_PROPERTY(UFloatProperty, Z_Param_TimeToAdd);
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<UFortMissionTimerComponent*>(Context)->AddOrRemoveTime(Z_Param_TimeToAdd);
    P_NATIVE_END;
}

void UFortMissionTimerComponent::HookNatives()
{
    // 9.00 (CL-6337466): AddOrRemoveTime's exec (0x1A68420, folded with UFortCheatManager::execEncounterSetDifficultyLevel)
    // steps the float and calls an empty function (0x32B4B0), so Wargames' Time Cheats (TimeCollectible_C) took no time
    // off the wave's defense timer.
    UFunction* Function = FindStaticFunction_AddOrRemoveTime();
    if (!Function || !Member_OnMissionTimerTimeAddedOrRemoved.IsAvailable() || !Member_TimerData.IsAvailable() || !FMissionTimerData::HasLayout()
        || !UKismetSystemLibrary::FindStaticFunction_K2_SetTimerDelegate() || !UKismetSystemLibrary::FindStaticFunction_K2_PauseTimerHandle())
    {
        return;
    }
    uint8_t* NativeCall = Finder::ImplementationCall(Function);
    if (!NativeCall || !Finder::IsStubFunction(Finder::RelativeTarget(reinterpret_cast<uintptr_t>(NativeCall), 1, 5)))
    {
        Log::Info(L"Finder: UFortMissionTimerComponent::AddOrRemoveTime is native in this build");
        return;
    }
    Function->SetNativeFunc(&execAddOrRemoveTime);
    Log::Info(L"Hook UFortMissionTimerComponent::AddOrRemoveTime: native (timer delegate +0x{:X}, handle +0x{:X} of TimerData)",
        FMissionTimerData::TimerDelegateOffset(), FMissionTimerData::TimerHandleOffset());
}
