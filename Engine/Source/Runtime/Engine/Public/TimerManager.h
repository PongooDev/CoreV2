#pragma once

#include "Delegates/Delegate.h"
#include "Engine/EngineTypes.h"

// DECLARE_DELEGATE(FTimerDelegate): what a timer calls. Core binds its own with FTimerDelegate::CreateLambda.
using FTimerDelegate = TBaseDelegate<void>;

// The engine's timer manager (Engine/Public/TimerManager.h): the game instance's, which UWorld::GetTimerManager hands
// out and which the world ticks once a frame (UWorld::Tick -> FTimerManager::Tick), so a timer runs on the game thread,
// in game time, with the rest of the world's timers. Core sets its timers through the build's own
// FTimerManager::InternalSetTimer (SetTimer is inline in Epic's header): the delegate goes in as an
// FTimerUnifiedDelegate's FuncDelegate, as SetTimer(FTimerHandle&, FTimerDelegate const&, ...) does.
//
// A delegate Core makes has no UObject, so the timer manager never clears it with an object
// (ClearAllTimersForObject): a Core timer checks what it works on (a weak pointer) when it runs, and one that has to
// stop simply doesn't set itself again. The game instance's timer manager outlives the world.
class FTimerManager
{
public:
    // Sets a timer to call InDelegate every InRate seconds when InbLoop, else once after InRate; the first call comes
    // after InFirstDelay when that is 0 or more. A timer already in InOutHandle is replaced; InRate <= 0 clears it.
    void SetTimer(FTimerHandle& InOutHandle, FTimerDelegate const& InDelegate, float InRate, bool InbLoop, float InFirstDelay = -1.0f);

    // Finds InternalSetTimer and the timer manager pointers UWorld::GetTimerManager reads, from
    // UKismetSystemLibrary::K2_SetTimerDelegate (the function that logs "SetTimer passed a bad function (%s) or object
    // (%s)"). Run on the game thread, once the engine's reflection is up. False where they weren't found: SetTimer then
    // runs its timers from Core's game thread queue instead (Fallback).
    static bool ResolveNatives();

    // The timer manager UWorld::GetTimerManager returns where ResolveNatives failed (or the world has none): its
    // SetTimer schedules on Core's game thread queue by wall-clock time.
    static FTimerManager& Fallback();
};
