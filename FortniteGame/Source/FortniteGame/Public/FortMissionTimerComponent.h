#pragma once

#include "Components/ActorComponent.h"
#include "UObject/ScriptDelegates.h"

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

public:
    // The timer's state was worked out again, with the component (5.30 0xEC68A0): SetMissionTimer, PauseMissionTimer and
    // the timer's periodic update on the server, OnRep_TimerData on a client. A Blueprint delegate.
    DefineUProperty(FMulticastScriptDelegate, OnMissionTimerComponentUpdated)
    static bool HasOnMissionTimerComponentUpdated() { return Member_OnMissionTimerComponentUpdated.IsAvailable(); }

    void PauseMissionTimer();
    void UnpauseMissionTimer();
    bool IsMissionTimerSet() const;
    bool IsMissionTimerPaused() const;
    float GetMissionTimerTimeRemaining() const;
};
