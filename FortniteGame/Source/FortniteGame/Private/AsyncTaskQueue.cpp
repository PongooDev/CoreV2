#include "pch.h"
#include "AsyncTaskQueue.h"
#include "Natives.h"
#include "UObject/WeakObjectPtr.h"

bool UAsyncTaskQueue::IsAvailable()
{
    return StaticClass() && Natives::UAsyncTaskQueue_Add && Natives::UAsyncTaskQueue_Start;
}

void UAsyncTaskQueue::Add(UObject* Owner, const FAsyncTaskDelegate& Task)
{
    if (!Natives::UAsyncTaskQueue_Add || !Task.IsBound())
    {
        return;
    }
    // The owner by reference (7.00 reads its index and serial number off rdx), the delegate copied into the new task.
    const FWeakObjectPtr WeakOwner(Owner);
    Natives::UAsyncTaskQueue_Add(this, WeakOwner, &Task);
}

void UAsyncTaskQueue::Start()
{
    if (Natives::UAsyncTaskQueue_Start)
    {
        Natives::UAsyncTaskQueue_Start(this);
    }
}

int32_t UAsyncTaskQueue::CurrentTaskOffset()
{
    return Member_CurrentTask.IsAvailable() ? Member_CurrentTask.GetOffsetChecked() : -1;
}
