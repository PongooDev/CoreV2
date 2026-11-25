#pragma once

#include "Components/ActorComponent.h"
#include "UObject/ScriptDelegates.h"
#include "UObject/Stack.h"

// The timer's replicated state (UFortMissionTimerComponent::TimerData). Its timer delegate (FTimerDynamicDelegate) and
// handle follow ClientRemainingTime unreflected (9.00 CL-6337466: +0x18 and +0x28 of 0x30; PauseMissionTimer 0x142F310
// reads them at the component's +0x128 and +0x138).
struct FMissionTimerData
{
    DefineUnrealStruct(FMissionTimerData, FortniteGame)
    DefineUProperty(bool, bTimerIsPaused)
    DefineUProperty(float, ReplicatedRemainingTime)
    DefineUProperty(float, TimeAddedOrRemoved)
    DefineUProperty(float, LastTimeAddedOrRemoved)
    DefineUProperty(float, ClientRemainingTime)

public:
    // The unreflected timer delegate's and handle's offsets in the struct, or -1 where the build's struct isn't laid out
    // as 9.00's (the delegate 8-aligned after ClientRemainingTime, the handle after it, nothing after the handle).
    static int32_t TimerDelegateOffset();
    static int32_t TimerHandleOffset();
    static bool HasLayout()
    {
        return Member_bTimerIsPaused.IsAvailable() && Member_ReplicatedRemainingTime.IsAvailable() && Member_TimeAddedOrRemoved.IsAvailable()
            && Member_LastTimeAddedOrRemoved.IsAvailable() && TimerDelegateOffset() >= 0;
    }
};

// Save the World's mission timer, the countdown at the top of the screen: a mission's (AFortMission::TimerComponent) or
// an objective's (AFortObjectiveBase::TimerComponent, SetObjectiveTimer). SetMissionTimer starts it on the world's
// timer manager with the mission blueprint's callback (TimerData.TimerDelegate), which fails the objective when it runs
// out; PauseMissionTimer and UnpauseMissionTimer pause and resume that timer (FTimerManager::PauseTimer, its remaining
// time kept) and replicate TimerData.bTimerIsPaused, which stops the HUD's countdown. Intact in shipping builds (1.10
// 0x640CC0 / 0x64AD60, 2.1 likewise), on the server only (owner's role Authority) and only while a timer is set.
class UFortMissionTimerComponent : public UActorComponent
{
    DefineUnrealClass(UFortMissionTimerComponent, FortniteGame)
    DefineUFunction(PauseMissionTimer)
    DefineUFunction(UnpauseMissionTimer)
    DefineUFunction(IsMissionTimerSet)
    DefineUFunction(IsMissionTimerPaused)
    DefineUFunction(GetMissionTimerTimeRemaining)
    DefineUFunction(AddOrRemoveTime)

public:
    // The timer's state was worked out again, with the component (5.30 0xEC68A0): SetMissionTimer, PauseMissionTimer and
    // the timer's periodic update on the server, OnRep_TimerData on a client. A Blueprint delegate.
    DefineUProperty(FMulticastScriptDelegate, OnMissionTimerComponentUpdated)
    static bool HasOnMissionTimerComponentUpdated() { return Member_OnMissionTimerComponentUpdated.IsAvailable(); }
    // 9.00: a Blueprint delegate (TimerComponent, TimeAdded) for time added to or taken off a running timer.
    DefineUProperty(FMulticastScriptDelegate, OnMissionTimerTimeAddedOrRemoved)
    DefineUStructProperty(FMissionTimerData, TimerData)

    void PauseMissionTimer();
    void UnpauseMissionTimer();
    bool IsMissionTimerSet() const;
    bool IsMissionTimerPaused() const;
    float GetMissionTimerTimeRemaining() const;

    // 9.00: moves the running timer's end by TimeToAdd seconds (Wargames' Time Cheats take time off the wave's
    // defense timer). Stripped from the build's server code (the exec calls an empty function).
    void AddOrRemoveTime(float TimeToAdd);
    DECLARE_FUNCTION(execAddOrRemoveTime);

    static void HookNatives();
};
