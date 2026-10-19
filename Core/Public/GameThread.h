#pragma once

#include <functional>

namespace GameThread
{
    bool Enqueue(std::function<void()> Task);
    bool RunAndWait(std::function<void()> Task);
    void Tick();
    bool IsInGameThread();
}
