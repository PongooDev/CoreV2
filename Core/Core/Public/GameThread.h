#pragma once

#include <functional>

namespace GameThread
{
    void Enqueue(std::function<void()> Task);
    void RunAndWait(std::function<void()> Task);
    void Tick();
    bool IsInGameThread();
}
