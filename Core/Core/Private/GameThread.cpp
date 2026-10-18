#include "pch.h"
#include "GameThread.h"
#include "Log.h"

#include <atomic>
#include <future>
#include <mutex>

namespace
{
    std::mutex QueueMutex;
    std::vector<std::function<void()>> Pending;
    std::atomic<DWORD> GameThreadId = 0;
}

void GameThread::Enqueue(std::function<void()> Task)
{
    std::scoped_lock Lock(QueueMutex);
    Pending.push_back(std::move(Task));
}

void GameThread::RunAndWait(std::function<void()> Task)
{
    if (IsInGameThread())
    {
        Task();
        return;
    }

    auto Done = std::make_shared<std::promise<void>>();
    std::future<void> Finished = Done->get_future();
    Enqueue([Task = std::move(Task), Done]()
    {
        Task();
        Done->set_value();
    });
    Finished.wait();
}

void GameThread::Tick()
{
    GameThreadId = GetCurrentThreadId();

    std::vector<std::function<void()>> Tasks;
    {
        std::scoped_lock Lock(QueueMutex);
        Tasks.swap(Pending);
    }

    for (std::function<void()>& Task : Tasks)
    {
        Task();
    }
}

bool GameThread::IsInGameThread()
{
    return GameThreadId != 0 && GameThreadId == GetCurrentThreadId();
}
