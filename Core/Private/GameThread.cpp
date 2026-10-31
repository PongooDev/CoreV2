#include "pch.h"
#include "GameThread.h"
#include "GameMapsSettings.h"
#include "Log.h"
#include "Natives.h"
#include "Engine/NetDriver.h"
#include "Engine/World.h"

#include <atomic>
#include <chrono>
#include <future>
#include <mutex>

namespace
{
    enum class ETaskState : uint8_t
    {
        Pending,
        Running,
        Abandoned,
    };

    std::mutex QueueMutex;
    std::vector<std::function<void()>> Pending;
    std::atomic<DWORD> GameThreadId = 0;
    bool bReadyLogged = false;

    bool IsRequestingExit()
    {
        return Natives::GIsRequestingExit && *Natives::GIsRequestingExit;
    }

#if WITH_SERVER_CODE
    void LogReady()
    {
        UWorld* World = GWorld;
        if (!World)
        {
            Log::Error(L"No world after the first engine tick");
            return;
        }

        Log::Info(L"Game default map {}, global default game mode {}", UGameMapsSettings::GetGameDefaultMap().ToStdString(), UGameMapsSettings::GlobalDefaultGameModePath().ToStdString());

        UNetDriver* NetDriver = World->NetDriver;
        Log::Info(L"Server ready: {}, net mode {}, net driver {}, game mode {}",
            World->GetFullName().ToStdString(),
            static_cast<int32_t>(World->InternalGetNetMode()),
            NetDriver ? NetDriver->GetFullName().ToStdString() : L"none",
            World->AuthorityGameMode ? reinterpret_cast<UObject*>(World->AuthorityGameMode)->GetFullName().ToStdString() : L"none");
    }
#endif
}

bool GameThread::Enqueue(std::function<void()> Task)
{
    if (IsRequestingExit())
    {
        Log::Warning(L"GameThread: the engine is exiting, task dropped");
        return false;
    }

    std::scoped_lock Lock(QueueMutex);
    Pending.push_back(std::move(Task));
    return true;
}

bool GameThread::RunAndWait(std::function<void()> Task)
{
    if (IsInGameThread())
    {
        Task();
        return true;
    }

    auto State = std::make_shared<std::atomic<ETaskState>>(ETaskState::Pending);
    auto Done = std::make_shared<std::promise<void>>();
    std::future<void> Finished = Done->get_future();
    const bool bQueued = Enqueue([Task = std::move(Task), State, Done]()
    {
        ETaskState Expected = ETaskState::Pending;
        if (State->compare_exchange_strong(Expected, ETaskState::Running))
        {
            Task();
            Done->set_value();
        }
    });
    if (!bQueued)
    {
        return false;
    }

    while (Finished.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready)
    {
        ETaskState Expected = ETaskState::Pending;
        if (IsRequestingExit() && State->compare_exchange_strong(Expected, ETaskState::Abandoned))
        {
            Log::Warning(L"GameThread: the engine is exiting, waiting task abandoned");
            return false;
        }
    }
    return true;
}

void GameThread::Tick()
{
    GameThreadId = GetCurrentThreadId();

#if WITH_SERVER_CODE
    if (!bReadyLogged)
    {
        bReadyLogged = true;
        LogReady();
    }
#endif

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
    if (Natives::GIsGameThreadIdInitialized && *Natives::GIsGameThreadIdInitialized && Natives::GGameThreadId)
    {
        return GetCurrentThreadId() == *Natives::GGameThreadId;
    }
    return GameThreadId != 0 && GameThreadId == GetCurrentThreadId();
}
