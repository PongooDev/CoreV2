#pragma once

#include "Delegates/Delegate.h"
#include "Reflection.h"
#include "UObject/Object.h"
#include "UObject/ScriptDelegates.h"

// One step of a UAsyncTaskQueue (from 6.20's playspaces): the owner it runs for and the delegate that starts it.
class UAsyncTask : public UObject
{
    DefineUnrealClass(UAsyncTask, FortniteGame)
};

// A queue of async steps that run one after another (from 6.20; 7.00 keeps one per island space, AFortVolume::TaskQueue).
// A step is a delegate that starts it and returns the delegate its end broadcasts (an FAsyncTaskResult), or null when it
// is done at once; the queue binds its OnAsyncTaskFinished to that and runs the next step then. Epic's island loads and
// resets are built on it: UFortLevelSaveComponent::BeginRestrictedPlotLoad (7.00 0xF363B0) and
// AFortVolumeManager::ResetVolumeAndLevelRecord (0xDA0E70) add their steps and start the queue.
class UAsyncTaskQueue : public UObject
{
    DefineUnrealClass(UAsyncTaskQueue, FortniteGame)

public:
    // The step running now (null when the queue is idle).
    DefineUProperty(UAsyncTask*, CurrentTask)

    // TDelegate<FOnAsyncTaskFinished*()>: starts a step, and returns the delegate its end broadcasts.
    using FAsyncTaskDelegate = TBaseDelegate<FMulticastScriptDelegate*>;

    // Whether the build's queue was found (Natives::UAsyncTaskQueue_Add and _Start, from 7.00).
    static bool IsAvailable();

    // UAsyncTaskQueue::Add (21.40's name; 7.00 0xBA90E0): a step for Owner (skipped if Owner is gone when its turn
    // comes), at the end of the queue.
    void Add(UObject* Owner, const FAsyncTaskDelegate& Task);
    // The queue's start (7.00 0xBADE10): the next step runs unless one is running already.
    void Start();

    // UAsyncTaskQueue::CurrentTask's offset (-1 without it), for the finder of Start.
    static int32_t CurrentTaskOffset();
};
